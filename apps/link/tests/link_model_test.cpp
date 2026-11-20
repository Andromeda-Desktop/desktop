// Model-based test: random event sequences against the feed, presence and transfer rules, with the invariants checked
// after every event.
#include "link_model.h"

#include <cstdio>
#include <ctime>
#include <format>
#include <ranges>
#include <tuple>
#include <cstdlib>
#include <random>
#include <set>

using namespace linkapp;

namespace {

  int failures = 0;

  void expect(bool ok, const char* what, unsigned seed, int step) {
    if (ok) return;
    std::fprintf(stderr, "FAIL seed %u step %d: %s\n", seed, step, what);
    ++failures;
  }

  void feedInvariants(const Feed& feed, unsigned seed, int step) {
    const auto& items = feed.items();
    expect(items.size() <= Feed::kMax, "at most kMax items", seed, step);
    std::set<std::string> keys;
    for (std::size_t i = 0; i < items.size(); ++i) {
      expect(keys.insert(items[i].key).second, "one item per key", seed, step);
      if (i > 0) expect(items[i - 1].atMs >= items[i].atMs, "newest first", seed, step);
    }
    const std::int64_t now = items.empty() ? 0 : items.front().atMs;
    const std::size_t split = feed.earlierStart(now);
    for (std::size_t i = 0; i < items.size(); ++i) {
      expect((i < split) == (now - items[i].atMs < Feed::kNowMs), "Now/Earlier split", seed, step);
    }
  }

  void runFeed(unsigned seed) {
    std::mt19937 rng(seed);
    Feed feed;
    std::uniform_int_distribution<int> op(0, 9), key(0, 300), age(0, 3 * 3600 * 1000);
    const std::int64_t base = 1'800'000'000'000;
    for (int step = 0; step < 4000; ++step) {
      const std::string k = "n:" + std::to_string(key(rng));
      const int o = op(rng);
      if (o < 5) {
        FeedItem item;
        item.key = k;
        item.kind = (key(rng) % 2) ? FeedItem::Kind::Offer : FeedItem::Kind::Notification;
        item.atMs = base - age(rng);
        feed.upsert(item);
        expect(feed.find(k).has_value() || feed.items().size() == Feed::kMax, "upserted item present", seed, step);
      } else if (o < 7) {
        const bool had = feed.find(k).has_value();
        expect(feed.remove(k) == had, "remove reports presence", seed, step);
        expect(!feed.find(k), "removed item gone", seed, step);
      } else {
        const auto to = static_cast<Transfer>(key(rng) % 4);
        const auto at = feed.find(k);
        const auto before = at ? std::optional(feed.items()[*at]) : std::nullopt;
        const bool moved = feed.moveTransfer(k, to, 5, 10);
        const bool allowed = before && before->kind == FeedItem::Kind::Offer && transferMayMove(before->transfer, to);
        expect(moved == allowed, "transfer moves only by the rules", seed, step);
        if (before && !moved) expect(feed.items()[*at].transfer == before->transfer, "rejected move changes nothing", seed, step);
      }
      feedInvariants(feed, seed, step);
    }
  }

  void presence() {
    using K = Presence::Kind;
    const Phone away{.id = "a", .name = "A"};
    const Phone live{.id = "b", .name = "B", .connected = true, .locked = 0};
    Phone locked = live;
    locked.locked = 1;
    // Every combination: the result names a phone exactly when one is paired and no pairing runs.
    for (int daemon = 0; daemon < 2; ++daemon) {
      for (int pairing = 0; pairing < 2; ++pairing) {
        for (const auto& phones : {std::vector<Phone>{}, {away}, {away, live}, {away, locked}}) {
          const Presence p = presenceFrom(daemon, pairing, phones);
          const bool named = !p.phone.id.empty();
          expect(named == (daemon && !pairing && !phones.empty()), "phone named only when shown", 0, 0);
          if (!daemon) expect(p.kind == K::NoDaemon, "no daemon wins", 0, 0);
          if (named) {
            expect(p.phone.connected == (p.kind == K::Connected || p.kind == K::Locked), "connected kinds", 0, 0);
            expect((p.kind == K::Locked) == (p.phone.locked == 1), "locked only when the phone says so", 0, 0);
          }
        }
      }
    }
  }

  // Random Texts snapshots, receipts, views and sends against a reference unread set and the conversation rules.
  void runMessages(unsigned seed) {
    using K = ConversationRow::Kind;
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> op(0, 7), threadOf(1, 6), pick(0, 39);
    const std::int64_t now = 1'763'600'000'000; // 2025-11-20 UTC
    std::vector<Text> universe;
    for (std::uint64_t id = 1; id <= 40; ++id) {
      const auto thread = static_cast<std::uint64_t>(threadOf(rng));
      universe.push_back({.id = id, .thread = thread, .address = std::format("+49 151 000{}", thread),
                          .body = std::format("t{}", id), .atMs = now - static_cast<std::int64_t>(pick(rng)) * 7 * 3600 * 1000,
                          .sent = pick(rng) % 3 == 0});
    }
    Messages m;
    std::vector<Text> shown;
    std::set<std::uint64_t> unread; // reference: received, not yet seen in a viewed thread
    std::optional<std::uint64_t> viewing;
    std::uint64_t textId = 1000;
    expect(m.load() == Messages::Load::Loading, "Loading before the first Texts", seed, 0);
    for (int step = 1; step <= 400; ++step) {
      switch (op(rng)) {
      case 0:
      case 1: { // a new snapshot: a random subset, plus the sent copies of what is sending
        shown.clear();
        for (const Text& t : universe) {
          if (pick(rng) % 2 == 0) shown.push_back(t);
        }
        for (const Thread& t : m.threads()) {
          for (const ConversationRow& row : m.conversation(t.id, now)) {
            if (row.kind == K::Sending && pick(rng) % 2 == 0) {
              shown.push_back({.id = ++textId, .thread = t.id, .address = t.address, .body = row.text, .atMs = now, .sent = true});
            }
          }
        }
        m.setTexts(shown);
        for (const Text& t : shown) {
          for (const Thread& th : m.threads()) {
            for (const ConversationRow& row : m.conversation(th.id, now)) {
              if (t.sent && th.id == t.thread && row.kind == K::Sending) {
                expect(row.text != t.body, "a sent text Texts holds leaves the outgoing list", seed, step);
              }
            }
          }
        }
        break;
      }
      case 2: {
        const std::uint64_t id = static_cast<std::uint64_t>(pick(rng)) + 1;
        m.received(id);
        unread.insert(id);
        break;
      }
      case 3:
        viewing = pick(rng) % 4 == 0 ? std::nullopt : std::optional<std::uint64_t>(threadOf(rng));
        m.view(viewing);
        break;
      case 4:
        m.setContacts({{.name = "Maya", .numbers = {"0151 0001"}}, {.name = "Dad", .numbers = {"+491510002", "0151 0003"}}});
        break;
      case 5: {
        const auto thread = static_cast<std::uint64_t>(threadOf(rng));
        const std::uint64_t serial = m.send(thread, std::format("s{}", step), now);
        expect((serial != 0) == (m.thread(thread) != nullptr), "send needs a known thread", seed, step);
        break;
      }
      default: {
        const auto serial = static_cast<std::uint64_t>(pick(rng) % 8) + 1;
        const Outgoing* out = m.outgoing(serial);
        const auto before = out ? std::optional(out->state) : std::nullopt;
        const bool fail = pick(rng) % 2 == 0;
        const bool moved = fail ? m.sendFailed(serial) : m.retry(serial);
        const auto want = fail ? Outgoing::State::Sending : Outgoing::State::Failed;
        expect(moved == (before == want), "Sending -> Failed and Failed -> Sending only", seed, step);
        break;
      }
      }
      if (viewing) {
        for (const Text& t : shown) {
          if (t.thread == *viewing) unread.erase(t.id);
        }
      }

      const auto& threads = m.threads();
      std::set<std::uint64_t> ids;
      for (std::size_t i = 0; i < threads.size(); ++i) {
        const Thread& th = threads[i];
        expect(ids.insert(th.id).second, "one row per thread", seed, step);
        if (i > 0) expect(threads[i - 1].atMs >= th.atMs, "threads newest first", seed, step);
        int want = 0;
        const Text* newest = nullptr;
        for (const Text& t : shown) {
          if (t.thread != th.id) continue;
          if (!t.sent && unread.contains(t.id)) ++want;
          if (newest == nullptr || std::tie(t.atMs, t.id) > std::tie(newest->atMs, newest->id)) newest = &t;
        }
        expect(th.unread == want, "unread = received texts not yet seen in a viewed thread", seed, step);
        expect(newest != nullptr && th.last == newest->body, "the row shows the newest text", seed, step);
        const std::string expectName = numberKey(th.address) == numberKey("0151 0001") ? "Maya" : "";
        if (!expectName.empty() && th.name != th.address) expect(th.name == expectName, "contact name by number", seed, step);
        if (viewing && th.id == *viewing) expect(th.unread == 0, "the viewed thread has no unread", seed, step);

        const auto rows = m.conversation(th.id, now);
        std::size_t bubbles = 0;
        for (std::size_t r = 0; r < rows.size(); ++r) {
          if (rows[r].kind == K::Day) {
            expect(r == 0 || rows[r - 1].runEnd, "the bubble before a day line ends its run", seed, step);
            expect(r + 1 < rows.size() && rows[r + 1].kind != K::Day, "a day line has bubbles", seed, step);
            continue;
          }
          ++bubbles;
          if (r > 0) expect(rows[r - 1].atMs <= rows[r].atMs, "oldest first", seed, step);
        }
        std::size_t want2 = 0;
        for (const Text& t : shown) want2 += t.thread == th.id ? 1 : 0;
        for (std::uint64_t serial = 1; serial <= 400; ++serial) {
          if (const Outgoing* out = m.outgoing(serial); out && out->thread == th.id) ++want2;
        }
        expect(bubbles == want2, "one bubble per text and per outgoing text", seed, step);
        expect(rows.empty() || rows.back().runEnd, "the last bubble ends its run", seed, step);
      }
      if (!shown.empty() || m.load() == Messages::Load::Ready) {
        std::set<std::uint64_t> want(std::from_range, shown | std::views::transform(&Text::thread));
        expect(ids == want, "a thread per thread id in Texts", seed, step);
      }
    }
  }

  void dialing() {
    Messages m;
    m.setContacts({{.name = "Maya Okafor", .numbers = {"+49 151 1234-0001"}}, {.name = "Mum", .numbers = {}},
                   {.name = "Mumbai Office", .numbers = {"(022) 555 0100"}}});
    expect(m.numberFor("+49 (151) 2345 6789") == "+4915123456789", "a typed number keeps digits and +", 0, 0);
    expect(m.numberFor("maya") == "+4915112340001", "a name prefix dials the contact's number", 0, 0);
    expect(m.numberFor("Mum") == "0225550100", "a contact without numbers is skipped", 0, 0);
    expect(m.numberFor("Zed").empty(), "an unknown name dials nothing", 0, 0);
    expect(duration(41) == "0:41" && duration(768) == "12:48" && duration(3723) == "1:02:03", "durations", 0, 0);
  }

  void days() {
    const std::int64_t now = 1'763'640'000'000; // Thu 2025-11-20 12:00 UTC
    const std::int64_t day = 86'400'000;
    expect(dayLabel(now, now) == "Today", "Today", 0, 0);
    expect(dayLabel(now - day, now) == "Yesterday", "Yesterday", 0, 0);
    expect(dayLabel(now - 3 * day, now) == "Monday", "weekday within a week", 0, 0);
    expect(dayLabel(now - 9 * day, now) == "11 Nov", "date this year", 0, 0);
    expect(dayLabel(now - 365 * day, now) == "20 Nov 2024", "date with year", 0, 0);
  }

} // namespace

int main() {
  setenv("TZ", "UTC", 1);
  tzset();
  for (unsigned seed = 1; seed <= 20; ++seed) runFeed(seed);
  for (unsigned seed = 1; seed <= 20; ++seed) runMessages(seed);
  presence();
  days();
  dialing();
  if (failures == 0) std::puts("link_model: ok");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
