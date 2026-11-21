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

// Random open/close/expire sequences against the window count DeskApp keeps; checks the invariants on every step.
int main() {
  for (unsigned seed = 0; seed < 200; ++seed) {
    std::mt19937 rng(seed);
    const bool hide = (seed & 1U) != 0;
    Lifecycle state = Lifecycle::Active;
    int windows = 0;
    bool quit = false;
    for (int step = 0; step < 200 && !quit; ++step) {
      LifecycleEvent event{};
      switch (rng() % 3) {
      case 0:
        ++windows;
        event = LifecycleEvent::WindowOpened;
        break;
      case 1:
        if (windows == 0) {
          continue;
        }
        if (--windows > 0) {
          continue;
        }
        event = LifecycleEvent::LastWindowClosed;
        break;
      default:
        event = LifecycleEvent::Expired;
        break;
      }
      const LifecycleStep next = lifecycleStep(state, event, hide);
      switch (next) {
      case LifecycleStep::Ignore:
        break;
      case LifecycleStep::Hide:
        state = Lifecycle::Hidden;
        break;
      case LifecycleStep::Show:
        state = Lifecycle::Active;
        break;
      case LifecycleStep::Quit:
        quit = true;
        break;
      }
      expect(state != Lifecycle::Hidden || windows == 0, "hidden with a window open", seed, step);
      expect(state != Lifecycle::Hidden || hide, "hidden without opting in", seed, step);
      expect(state == Lifecycle::Hidden || windows > 0 || quit || event != LifecycleEvent::LastWindowClosed,
          "the last window closed and the app neither hid nor quit", seed, step);
      expect(!quit || windows == 0, "quit with a window open", seed, step);
      expect(!(event == LifecycleEvent::Expired && next == LifecycleStep::Quit) || hide,
          "an expiry quit an app that never hides", seed, step);
    }
  }
  return g_failures == 0 ? 0 : 1;
}
