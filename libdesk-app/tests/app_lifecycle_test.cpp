#include "app/app_lifecycle.h"

#include <print>
#include <random>

namespace {
  int g_failures = 0;

  void expect(bool ok, const char* what, unsigned seed, int step) {
    if (!ok) {
      std::println(stderr, "app_lifecycle_test: {} (seed {}, step {})", what, seed, step);
      ++g_failures;
    }
  }
} // namespace

// Random open/close/expire/lease/quit sequences against the window and lease counts DeskApp keeps; checks the
// invariants on every step.
int main() {
  for (unsigned seed = 0; seed < 400; ++seed) {
    std::mt19937 rng(seed);
    const bool hide = (seed & 1U) != 0;
    Lifecycle state = Lifecycle::Active;
    int windows = 0;
    int leases = 0;
    for (int step = 0; step < 300; ++step) {
      LifecycleEvent event{};
      const unsigned pick = rng() % 16;
      if (pick < 4) {
        ++windows;
        event = LifecycleEvent::WindowOpened;
      } else if (pick < 8) {
        if (windows == 0 || --windows > 0) {
          continue;
        }
        event = LifecycleEvent::LastWindowClosed;
      } else if (pick < 10) {
        event = LifecycleEvent::Expired;
      } else if (pick < 12) {
        ++leases;
        continue;
      } else if (pick < 15) {
        if (leases == 0) {
          continue;
        }
        --leases;
        event = LifecycleEvent::LeaseReleased;
      } else if (rng() % 8 == 0) {
        event = LifecycleEvent::Quit;
      } else {
        continue;
      }
      const Lifecycle from = state;
      const LifecycleStep next =
          lifecycleStep(from, event, {.hideWhenClosed = hide, .windowOpen = windows > 0, .leases = leases});
      state = lifecycleAfter(next, from);
      expect(from != Lifecycle::Quitting || next == LifecycleStep::Ignore, "an event after Quitting", seed, step);
      expect(next != LifecycleStep::Quit || event == LifecycleEvent::Quit || (windows == 0 && leases == 0),
          "quit with a window or a lease open", seed, step);
      expect(state == Lifecycle::Active || state == Lifecycle::Quitting || windows == 0, "hidden with a window open",
          seed, step);
      expect(state != Lifecycle::Hidden || hide, "hidden without opting in", seed, step);
      expect(state != Lifecycle::HiddenExpired || leases > 0, "waiting with no lease left", seed, step);
      expect(event != LifecycleEvent::LastWindowClosed || from != Lifecycle::Active || state != Lifecycle::Active,
          "the last window closed and the app neither hid nor quit", seed, step);
      expect(event != LifecycleEvent::Expired || windows > 0 || leases > 0 || state == Lifecycle::Quitting,
          "expired with nothing open and did not quit", seed, step);
      expect(event != LifecycleEvent::WindowOpened || state == Lifecycle::Active || state == Lifecycle::Quitting,
          "a window opened and the app stayed hidden", seed, step);
    }
  }
  return g_failures == 0 ? 0 : 1;
}
