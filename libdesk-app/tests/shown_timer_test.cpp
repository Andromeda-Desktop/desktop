#include "core/shown_timer.h"

#include <array>
#include <cstdio>
#include <random>

// Random start, stop, show, hide and loop ticks: a tick runs only while the window is shown, each started timer
// ticks once when the window comes back, and a callback may stop its own timer.
int main() {
  int failures = 0;
  auto check = [&](bool ok, const char* what, unsigned seed, int step) {
    if (!ok) {
      std::fprintf(stderr, "FAIL seed %u step %d: %s\n", seed, step, what);
      ++failures;
    }
  };
  for (unsigned seed = 1; seed <= 300 && failures == 0; ++seed) {
    std::mt19937 rng(seed);
    ShownTimers window;
    std::array<ShownTimer, 3> timers;
    std::array<int, 3> calls{};
    std::array<bool, 3> selfStop{};
    for (int step = 1; step <= 300; ++step) {
      const std::size_t i = rng() % timers.size();
      const std::array<int, 3> before = calls;
      const bool wasShown = window.shown();
      std::array<bool, 3> wasActive{};
      for (std::size_t t = 0; t < timers.size(); ++t) {
        wasActive[t] = timers[t].active();
      }
      switch (rng() % 5) {
      case 0:
        selfStop[i] = rng() % 4 == 0;
        timers[i].startRepeating(window, std::chrono::milliseconds(0), [&, i] {
          check(window.shown(), "ticked while the window is not shown", seed, step);
          ++calls[i];
          if (selfStop[i]) {
            timers[i].stop();
          }
        });
        check(timers[i].active(), "a started timer is not active", seed, step);
        break;
      case 1:
        timers[i].stop();
        check(!timers[i].active() && !timers[i].ticking(), "a stopped timer still runs", seed, step);
        break;
      case 2:
      case 3: {
        const bool shown = rng() % 2 == 0;
        window.setShown(shown);
        for (std::size_t t = 0; t < timers.size(); ++t) {
          const int want = shown && !wasShown && wasActive[t] ? 1 : 0;
          check(calls[t] - before[t] == want, "a shown window did not tick each started timer once", seed, step);
        }
        break;
      }
      default:
        TimerManager::instance().tick();
        if (!window.shown()) {
          check(calls == before, "a loop tick ran while the window is not shown", seed, step);
        }
        break;
      }
      for (std::size_t t = 0; t < timers.size(); ++t) {
        check(timers[t].ticking() == (timers[t].active() && window.shown()), "loop timer differs from started and shown",
              seed, step);
      }
    }
    for (auto& timer : timers) {
      timer.stop();
    }
  }
  return failures == 0 ? 0 : 1;
}
