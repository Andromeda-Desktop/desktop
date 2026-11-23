#include "idle/presence_watch.h"
#include "tests/test_check.h"

#include <cstdint>
#include <optional>
#include <random>

// Random event sequences against a simulated clock; every step checks the invariants a user relies on.
namespace {

  struct World {
    PresenceWatch watch;
    PresenceWatch::Settings settings;
    bool locked = false;
    bool screenOn = true;
    bool inhibited = false;
    Presence presence = Presence::Unknown;
    std::int64_t now = 0;
    std::int64_t lastInput = 0;
    std::optional<std::int64_t> absentSince;
    std::optional<std::int64_t> deadline;
    bool sawAbsentWhileOff = false;

    [[nodiscard]] std::int64_t idle() const { return inhibited ? 0 : now - lastInput; }

    void apply(const PresenceWatch::Effects& effects) {
      if (effects.lock) {
        TEST_CHECK(settings.onLeave == PresenceWatch::Leave::Lock && !locked && screenOn && presence == Presence::Absent && !inhibited);
        const std::int64_t need = PresenceWatch::kDebounceSeconds + settings.leaveDelaySeconds;
        TEST_CHECK(absentSince.has_value() && now - *absentSince >= need);
        TEST_CHECK(now - lastInput >= need);
      }
      if (effects.wake) {
        TEST_CHECK(settings.wakeOnApproach && !screenOn && presence == Presence::Present && sawAbsentWhileOff);
      }
      if (effects.rearm) {
        const auto delay = watch.timerDelay();
        deadline = delay.has_value() ? std::optional(now + delay->count()) : std::nullopt;
      }
      TEST_CHECK(watch.claimed() == (watch.mode() != PresenceWatch::Mode::Off));
      TEST_CHECK(watch.claimed() || !watch.dimmed());
      if (settings.onLeave == PresenceWatch::Leave::Off && !settings.wakeOnApproach) {
        TEST_CHECK(!watch.claimed());
      }
      if (watch.dimmed()) {
        TEST_CHECK(settings.onLeave != PresenceWatch::Leave::Off && !locked && screenOn && presence == Presence::Absent);
      }
      if (watch.mode() == PresenceWatch::Mode::Waking || watch.mode() == PresenceWatch::Mode::Approach) {
        TEST_CHECK(settings.wakeOnApproach && !screenOn);
      }
      if (watch.mode() == PresenceWatch::Mode::Watching || watch.mode() == PresenceWatch::Mode::Leaving) {
        TEST_CHECK(settings.onLeave != PresenceWatch::Leave::Off && !locked && screenOn);
      }
      TEST_CHECK(deadline.has_value() == watch.timerDelay().has_value());
    }

    void elapse(std::int64_t seconds) {
      if (deadline.has_value() && now + seconds >= *deadline) {
        now = *deadline;
        apply(watch.timer(idle()));
        return;
      }
      now += seconds;
    }
  };

  void run(std::uint32_t seed) {
    std::mt19937 rng(seed);
    World w;
    for (int step = 0; step < 600; ++step) {
      switch (rng() % 9) {
      case 0: {
        const auto p = static_cast<Presence>(rng() % 3);
        if (p == Presence::Absent && w.presence != Presence::Absent) {
          w.absentSince = w.now;
        }
        if (p != Presence::Unknown) {
          w.presence = p;
        }
        if (p == Presence::Absent && !w.screenOn) {
          w.sawAbsentWhileOff = true;
        }
        if (p == Presence::Present) {
          w.absentSince.reset();
        }
        w.apply(w.watch.reading(p));
        break;
      }
      case 1:
        w.lastInput = w.now;
        w.apply(w.watch.activity());
        break;
      case 2:
        w.locked = !w.locked;
        w.apply(w.watch.setLocked(w.locked));
        break;
      case 3:
        w.screenOn = !w.screenOn;
        w.sawAbsentWhileOff = !w.screenOn && w.presence == Presence::Absent;
        w.apply(w.watch.setScreenOn(w.screenOn));
        break;
      case 4:
        w.inhibited = !w.inhibited;
        if (!w.inhibited) {
          w.lastInput = w.now;
        }
        break;
      case 5:
        w.settings = {.onLeave = static_cast<PresenceWatch::Leave>(rng() % 3), .wakeOnApproach = (rng() % 2) != 0,
                      .leaveDelaySeconds = 10 + static_cast<std::int64_t>(rng() % 50)};
        w.apply(w.watch.setSettings(w.settings));
        break;
      default:
        w.elapse(1 + static_cast<std::int64_t>(rng() % 40));
        break;
      }
    }
  }

  // The plain walk-away path reaches the lock: present, then away with no input for debounce + delay.
  void walkAwayLocks() {
    World w;
    w.settings = {.onLeave = PresenceWatch::Leave::Lock, .leaveDelaySeconds = 20};
    w.apply(w.watch.setSettings(w.settings));
    w.presence = Presence::Absent;
    w.absentSince = 0;
    w.apply(w.watch.reading(Presence::Absent));
    w.elapse(5);
    TEST_CHECK(w.watch.dimmed());
    w.now = *w.deadline;
    const auto effects = w.watch.timer(w.idle());
    w.apply(effects);
    TEST_CHECK(effects.lock);
  }

} // namespace

int main() {
  walkAwayLocks();
  for (std::uint32_t seed = 1; seed <= 300; ++seed) {
    run(seed);
  }
  return 0;
}
