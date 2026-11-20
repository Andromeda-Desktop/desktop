// Model-based test: random event sequences against the feed, presence and transfer rules, with the invariants checked
// after every event.
#include "link_model.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <format>
#include <ranges>
#include <tuple>
#include <cstdlib>
#include <map>
#include <optional>
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
    // Every combination: the result names a phone exactly when one is paired.
    for (int daemon = 0; daemon < 2; ++daemon) {
      for (const auto& phones : {std::vector<Phone>{}, {away}, {away, live}, {away, locked}}) {
        const Presence p = presenceFrom(daemon, phones);
        const bool named = !p.phone.id.empty();
        expect(named == (daemon && !phones.empty()), "phone named only when shown", 0, 0);
        if (!daemon) expect(p.kind == K::NoDaemon, "no daemon wins", 0, 0);
        if (named) {
          expect(p.phone.connected == (p.kind == K::Connected || p.kind == K::Locked), "connected kinds", 0, 0);
          expect((p.kind == K::Locked) == (p.phone.locked == 1), "locked only when the phone says so", 0, 0);
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

  // Random Transfer* signals: one entry per id, newest first, only allowed moves, declined offers gone.
  void runActivity(unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> op(0, 5), idOf(0, 11), statusOf(0, 6);
    const std::array<const char*, 7> statuses{"done", "failed", "declined", "no-space", "too-large", "busy", "cancelled"};
    Activity a;
    std::int64_t now = 1'000'000;
    for (int step = 1; step <= 300; ++step) {
      now += 250;
      const std::string id = std::format("t{}", idOf(rng));
      const auto before = std::ranges::find(a.items(), id, &TransferEntry::id);
      const std::optional<Transfer> was = before == a.items().end() ? std::nullopt : std::optional(before->state);
      const auto after = [&] {
        const auto it = std::ranges::find(a.items(), id, &TransferEntry::id);
        return it == a.items().end() ? std::nullopt : std::optional(it->state);
      };
      switch (op(rng)) {
      case 0:
        a.offered(id, "p1", {{"a.pdf", 1000}, {"b.jpg", 500}}, now);
        expect(after().has_value(), "an offer shows", seed, step);
        if (was) expect(after() == was, "a repeated offer changes nothing", seed, step);
        break;
      case 1:
      case 2: {
        const bool moved = a.progress(id, 300, 1500, now);
        expect(moved == (!was || transferMayMove(*was, Transfer::Running)), "progress only where Running is allowed", seed, step);
        if (moved) expect(after() == Transfer::Running, "progress means Running", seed, step);
        break;
      }
      case 3:
      case 4: {
        const std::string status = statuses[static_cast<std::size_t>(statusOf(rng))];
        const bool moved = a.finished(id, status, {}, now);
        if (status == "declined") {
          expect(moved == was.has_value() && !after(), "a declined transfer leaves", seed, step);
        } else {
          const Transfer to = status == "done" ? Transfer::Done : Transfer::Failed;
          expect(moved == (was && transferMayMove(*was, to)), "finish only by an allowed move", seed, step);
          if (moved) expect(after() == to, "finish sets the end state", seed, step);
        }
        break;
      }
      default:
        a.clearFinished();
        expect(a.finishedCount() == 0, "Clear finished keeps only open transfers", seed, step);
        break;
      }
      std::set<std::string> ids;
      for (std::size_t i = 0; i < a.items().size(); ++i) {
        const TransferEntry& e = a.items()[i];
        expect(ids.insert(e.id).second, "one entry per transfer", seed, step);
        if (i > 0) expect(a.items()[i - 1].atMs >= e.atMs, "newest first", seed, step);
        if (e.state == Transfer::Done) expect(e.bytes == e.total, "a done transfer is complete", seed, step);
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

  void runPair(unsigned seed) {
    using K = Pair::Kind;
    using E = PairEvent::Kind;
    const std::set<std::pair<K, K>> edges{{K::Idle, K::Starting},  {K::Failed, K::Starting}, {K::Starting, K::Showing},
                                          {K::Starting, K::Failed}, {K::Showing, K::Failed},  {K::Showing, K::Idle}};
    std::mt19937 rng(seed);
    Pair pair;
    for (int step = 0; step < 2000; ++step) {
      PairEvent event{.kind = static_cast<E>(rng() % 5)};
      if (event.kind == E::Started) event = {.kind = E::Started, .code = rng() % 4 ? std::to_string(rng() % 1000000) : "", .uri = "andromeda-link://pair"};
      if (event.kind == E::Failed) event = {.kind = E::Failed, .error = "No network"};
      const Pair before = pair;
      const bool moved = pairStep(pair, event);
      if (moved) {
        expect(edges.contains({before.kind, pair.kind}), "only allowed moves", seed, step);
      } else {
        expect(pair.kind == before.kind && pair.code == before.code && pair.error == before.error,
               "a rejected event changes nothing", seed, step);
      }
      expect((pair.kind == K::Showing) == !pair.code.empty(), "a code shows exactly while Showing", seed, step);
      expect(pair.kind == K::Failed || pair.error.empty(), "an error only while Failed", seed, step);
      expect(pair.kind == K::Showing || pair.uri.empty(), "a URI only while Showing", seed, step);
    }
  }

  // Random loads, failures, selections and save batches against the Photos rules.
  void runPhotos(unsigned seed) {
    using L = Photos::Load;
    std::mt19937 rng(seed);
    const std::int64_t now = 1'763'640'000'000;
    Photos photos;
    std::size_t batch = 0;
    for (int step = 0; step < 3000; ++step) {
      switch (rng() % 8) {
      case 0: (void)photos.startLoad(); break;
      case 1: {
        std::vector<Photo> list;
        std::set<int> used;
        for (unsigned i = rng() % 12; i > 0; --i) {
          const int id = static_cast<int>(rng() % 20);
          if (!used.insert(id).second) continue;
          list.push_back({.path = std::format("/p/{}", id), .name = "x", .atS = now / 1000 - static_cast<std::int64_t>(rng() % 900'000)});
        }
        (void)photos.loaded(std::move(list));
        break;
      }
      case 2: (void)photos.failed(static_cast<L>(rng() % 6)); break;
      case 3:
      case 4: photos.toggle(rng() % 14); break;
      case 5: photos.clearSelection(); break;
      case 6: {
        const bool saving = photos.save() == Photos::Save::Saving;
        const std::size_t want = photos.selectedCount();
        const auto paths = photos.startSave();
        expect(paths.size() == (saving ? 0 : want), "a save takes the whole selection unless one runs", seed, step);
        if (!paths.empty()) batch = paths.size();
        break;
      }
      case 7: photos.saveAnswered(rng() % 2); break;
      }
      const auto& items = photos.items();
      std::size_t picked = 0;
      for (std::size_t i = 0; i < items.size(); ++i) {
        picked += photos.selected(i) ? 1 : 0;
        if (i > 0) expect(items[i - 1].atS >= items[i].atS, "newest first", seed, step);
      }
      expect(picked == photos.selectedCount(), "the selection is a subset of the photos", seed, step);
      const L load = photos.load();
      if (load != L::Ready && load != L::Loading) expect(items.empty() && picked == 0, "no photos unless loaded", seed, step);
      const std::size_t answered = photos.saved() + photos.saveFailed();
      if (photos.save() == Photos::Save::Saving) expect(answered < batch, "saving until every photo answered", seed, step);
      if (photos.save() == Photos::Save::Done) expect(answered == batch, "done when every photo answered", seed, step);
      const auto days = photos.days(now);
      for (std::size_t d = 0; d < days.size(); ++d) {
        const std::size_t end = d + 1 < days.size() ? days[d + 1].first : items.size();
        expect(days[d].first < end, "each day has a photo", seed, step);
        for (std::size_t i = days[d].first; i < end; ++i) {
          expect(dayLabel(items[i].atS * 1000, now) == days[d].second, "a photo sits under its own day", seed, step);
        }
      }
      expect(items.empty() == days.empty() && (days.empty() || days.front().first == 0), "days start at the first photo", seed, step);
    }
  }

  // Random navigation with replies in any order, and save events, against the Files rules.
  void runFiles(unsigned seed) {
    using L = Files::Load;
    using S = FileSave::State;
    std::mt19937 rng(seed);
    Files files;
    std::vector<std::pair<std::uint64_t, std::string>> pending; // serial, path asked
    std::uint64_t newest = 0;
    std::string newestPath = "/";
    const std::array<std::string, 4> names{"DCIM", "b.pdf", "Alpha", "a.txt"};
    std::map<std::string, S> saves;
    for (int step = 0; step < 3000; ++step) {
      switch (rng() % 7) {
      case 0: newestPath = files.childPath(names[rng() % 4]); newest = files.go(newestPath); pending.emplace_back(newest, newestPath); break;
      case 1: newestPath = files.parent(); newest = files.go(newestPath); pending.emplace_back(newest, newestPath); break;
      case 2: {
        newestPath = files.crumbPath(rng() % 4);
        newest = files.go(newestPath);
        pending.emplace_back(newest, newestPath);
        break;
      }
      case 3:
      case 4: {
        if (pending.empty()) break;
        const std::size_t i = rng() % pending.size();
        const auto [serial, path] = pending[i];
        pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(i));
        std::vector<FileEntry> list;
        for (const auto& n : names) {
          if (rng() % 2) list.push_back({.name = n, .folder = !n.contains('.')});
        }
        const bool ok = rng() % 4 ? files.listed(serial, list) : files.failed(serial, "busy");
        expect(ok == (serial == newest && path == files.path()), "only the newest reply lands", seed, step);
        break;
      }
      case 5: {
        const std::string path = "/x/" + names[rng() % 4];
        const auto before = saves.contains(path) ? std::optional(saves[path]) : std::nullopt;
        const bool started = files.saveStarted(path, 10);
        expect(started == (!before || *before != S::Running), "one save per file at a time", seed, step);
        if (started) saves[path] = S::Running;
        break;
      }
      case 6: {
        const std::string path = "/x/" + names[rng() % 4];
        const bool running = saves.contains(path) && saves[path] == S::Running;
        const bool done = rng() % 2;
        const bool moved = done ? files.saveDone(path, "/d/" + path) : files.saveFailed(path);
        expect(moved == running, "a save ends only while it runs", seed, step);
        if (moved) saves[path] = done ? S::Done : S::Failed;
        break;
      }
      }
      expect(files.path() == newestPath, "the path is the newest asked", seed, step);
      expect(files.crumbs().size() == static_cast<std::size_t>(std::ranges::count(files.path(), '/')) + (files.path() == "/" ? 0 : 1),
             "one crumb per folder", seed, step);
      expect(files.crumbPath(files.crumbs().size() - 1) == files.path(), "the last crumb is the path", seed, step);
      const auto& entries = files.entries();
      for (std::size_t i = 1; i < entries.size(); ++i) {
        expect(entries[i - 1].folder >= entries[i].folder, "folders first", seed, step);
      }
      if (files.load() == L::Failed) expect(entries.empty() && !files.error().empty(), "a failed folder lists nothing", seed, step);
      for (const auto& [path, state] : saves) expect(files.save(path) && files.save(path)->state == state, "save states", seed, step);
    }
  }

  void runMirror(unsigned seed) {
    using K = Mirror::Kind;
    using E = MirrorEvent::Kind;
    const std::set<std::pair<K, K>> edges{{K::Idle, K::Asking},    {K::Ended, K::Asking},    {K::Failed, K::Asking},
                                          {K::Asking, K::Showing}, {K::Showing, K::Ended},   {K::Asking, K::Failed},
                                          {K::Showing, K::Failed}, {K::Asking, K::Idle},     {K::Showing, K::Idle}};
    std::mt19937 rng(seed);
    Mirror mirror;
    for (int step = 0; step < 3000; ++step) {
      // Events of the current session, or of an older one (a late reply).
      const std::uint64_t session = rng() % 3 ? mirror.session : mirror.session - std::min<std::uint64_t>(mirror.session, 1 + rng() % 2);
      const MirrorEvent event{.kind = static_cast<E>(rng() % 5), .session = session, .width = 1080, .height = 2400, .error = "denied"};
      const Mirror before = mirror;
      const bool moved = mirrorStep(mirror, event);
      if (moved) {
        expect(edges.contains({before.kind, mirror.kind}), "only allowed moves", seed, step);
        const bool sessionFree = event.kind == E::Start || event.kind == E::Stop;
        expect(sessionFree || session == before.session, "an older session's event never lands", seed, step);
        expect(mirror.session == before.session + (event.kind == E::Start ? 1 : 0), "Start alone opens a session", seed, step);
      } else {
        expect(mirror.kind == before.kind && mirror.session == before.session, "a rejected event changes nothing", seed, step);
      }
      expect((mirror.kind == K::Showing) == (mirror.width > 0), "a size exactly while Showing", seed, step);
      expect(mirror.kind == K::Failed || mirror.error.empty(), "an error only while Failed", seed, step);
    }
  }

  void lastSeen() {
    const std::int64_t now = 1'763'640'000; // 2025-11-20 12:00 UTC
    expect(lastSeenText(0, now) == "Not connected", "never seen", 0, 0);
    expect(lastSeenText(now - 20, now) == "Last seen just now", "just now", 0, 0);
    expect(lastSeenText(now - 12 * 60, now) == "Last seen 12 min ago", "minutes", 0, 0);
    expect(lastSeenText(now - 3 * 3600, now) == "Last seen 3 h ago", "hours", 0, 0);
    expect(lastSeenText(now - 86400, now) == "Last seen yesterday", "yesterday", 0, 0);
    expect(lastSeenText(now - 9 * 86400, now) == "Last seen 11 Nov", "date", 0, 0);
  }

} // namespace

int main() {
  for (unsigned seed = 1; seed <= 20; ++seed) runPair(seed);
  lastSeen();
  for (unsigned seed = 1; seed <= 20; ++seed) runPhotos(seed);
  for (unsigned seed = 1; seed <= 20; ++seed) runFiles(seed);
  for (unsigned seed = 1; seed <= 20; ++seed) runMirror(seed);
  setenv("TZ", "UTC", 1);
  tzset();
  for (unsigned seed = 1; seed <= 20; ++seed) runFeed(seed);
  for (unsigned seed = 1; seed <= 20; ++seed) runMessages(seed);
  presence();
  days();
  dialing();
  for (unsigned seed = 1; seed <= 20; ++seed) runActivity(seed);
  if (failures == 0) std::puts("link_model: ok");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
