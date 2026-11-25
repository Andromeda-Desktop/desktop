#include "notification/notification_manager.h"
#include "tests/test_check.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

// Random replace, close, expire, dismiss and suppress sequences from five owners: the manager's owner answer matches
// a model of who holds each id (active, suppressed, or expired waiting for its close), and no call crosses owners.
namespace {

  enum class Held : std::uint8_t { Active, Suppressed, Pending };

  struct Entry {
    NotificationOwner owner;
    Held held;
  };

  const std::vector<NotificationOwner> kOwners{
      {.kind = NotificationOwner::Kind::Shell},
      {.kind = NotificationOwner::Kind::Host},
      {.kind = NotificationOwner::Kind::App, .id = "org.a"},
      {.kind = NotificationOwner::Kind::App, .id = "org.b"},
      {.kind = NotificationOwner::Kind::Connection, .id = ":1.5"},
  };

  bool runSeed(std::uint32_t seed) {
    std::mt19937 rng(seed);
    const auto pick = [&rng](std::size_t n) { return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng); };
    NotificationManager manager;
    manager.setFilters({NotificationFilterConfig{.name = "mute", .match = "muted", .showToast = false, .saveHistory = false}});
    std::map<std::uint32_t, Entry> model;
    std::vector<std::uint32_t> seen{0};
    bool ok = true;
    const auto expect = [&](bool condition, int step, const char* what) {
      if (!condition && ok) {
        std::fprintf(stderr, "seed %u step %d: %s\n", seed, step, what);
        ok = false;
      }
    };

    for (int step = 0; step < 300 && ok; ++step) {
      const NotificationOwner& owner = kOwners[pick(kOwners.size())];
      const std::uint32_t target = seen[pick(seen.size())];
      const auto it = model.find(target);
      const bool owned = it != model.end() && it->second.owner == owner;
      switch (pick(4)) {
      case 0: { // Notify, maybe replacing
        const bool muted = pick(4) == 0;
        const std::uint32_t replaces = pick(2) == 0 ? target : 0;
        const bool replacing = replaces != 0 && owned;
        const std::uint32_t id = manager.addOrReplace(NotificationRequest{
            .replacesId = replaces,
            .appName = muted ? "muted" : "app",
            .summary = "s" + std::to_string(step),
            .owner = owner,
            .origin = NotificationOrigin::External,
            .actions = {"default", "Open"},
        });
        if (replacing && it->second.held == Held::Active) {
          expect(id == replaces, step, "an owned active id is replaced in place, muted or not");
          break;
        }
        if (replacing && it->second.held == Held::Suppressed && muted) {
          expect(id == replaces, step, "an owned suppressed id stays suppressed");
          break;
        }
        if (replacing) {
          model.erase(replaces); // suppressed now shown, or pending closed: a new id
        }
        expect(id != 0 && !model.contains(id), step, "a new id is fresh");
        model[id] = Entry{.owner = owner, .held = muted ? Held::Suppressed : Held::Active};
        seen.push_back(id);
        break;
      }
      case 1: { // fdo CloseNotification
        const bool closed = manager.isOwnedBy(target, owner) && manager.close(target, CloseReason::ClosedByCall);
        expect(closed == owned, step, "only the owner closes");
        if (owned) {
          model.erase(target);
        }
        break;
      }
      case 2: // expiry
        if (it != model.end() && it->second.held == Held::Active) {
          expect(manager.close(target, CloseReason::Expired), step, "an active id expires");
          it->second.held = Held::Pending;
        }
        break;
      default: // the user dismisses a pending one from history
        if (it != model.end() && it->second.held == Held::Pending) {
          expect(manager.close(target, CloseReason::Dismissed), step, "a pending id is dismissed");
          model.erase(it);
        }
        break;
      }

      for (const std::uint32_t id : seen) {
        const auto entry = model.find(id);
        for (const NotificationOwner& o : kOwners) {
          expect(
              manager.isOwnedBy(id, o) == (entry != model.end() && entry->second.owner == o), step,
              "isOwnedBy matches the model"
          );
        }
      }
    }
    return ok;
  }

} // namespace

int main() {
  for (std::uint32_t seed = 1; seed <= 20; ++seed) {
    TEST_CHECK(runSeed(seed));
  }
  return 0;
}
