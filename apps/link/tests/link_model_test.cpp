// Model-based test: random event sequences against the feed, presence and transfer rules, with the invariants checked
// after every event.
#include "link_model.h"

#include <cstdio>
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

} // namespace

int main() {
  for (unsigned seed = 1; seed <= 20; ++seed) runFeed(seed);
  presence();
  if (failures == 0) std::puts("link_model: ok");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
