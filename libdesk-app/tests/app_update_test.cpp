#include "app/app_update.h"

#include <print>
#include <random>

namespace {
  int g_failures = 0;

  void expect(bool ok, const char* what, unsigned seed, int step) {
    if (!ok) {
      std::println(stderr, "app_update_test: {} (seed {}, step {})", what, seed, step);
      ++g_failures;
    }
  }
} // namespace

// Random timer ticks, helper results (also stale ones with no child), clicks, quits and swap-start results (a spawn
// that fails as often as it starts) against the child, swap and row state the updater keeps; checks the invariants
// on every step.
int main() {
  for (unsigned seed = 0; seed < 400; ++seed) {
    std::mt19937 rng(seed);
    UpdatePhase phase = UpdatePhase::Idle;
    int children = 0;
    int swaps = 0;
    bool verified = false;
    for (int step = 0; step < 300; ++step) {
      const auto event = static_cast<UpdateEvent>(rng() % 7);
      const UpdatePhase from = phase;
      const bool childBefore = children > 0;
      UpdateStep next = updateStep(from, event);
      phase = next.next;
      bool swapFailed = false;
      switch (next.action) {
      case UpdateAction::SpawnCheck:
        ++children;
        break;
      case UpdateAction::ShowReady:
        verified = true;
        --children;
        break;
      case UpdateAction::StopCheck:
        --children;
        break;
      case UpdateAction::SwapAndReopen:
      case UpdateAction::SwapOnExit: {
        // The updater reports the spawn result before it returns, as MacUpdater::apply does.
        swapFailed = rng() % 2 == 0;
        const UpdateStep result = updateStep(phase, swapFailed ? UpdateEvent::SwapFailed : UpdateEvent::SwapStarted);
        expect(result.action == UpdateAction::None, "a swap result asked for an action", seed, step);
        phase = result.next;
        swaps += swapFailed ? 0 : 1;
        break;
      }
      case UpdateAction::None:
        if (from == UpdatePhase::Checking && phase == UpdatePhase::Idle) {
          --children;
        }
        break;
      }
      expect(phase != UpdatePhase::Swapping, "a swap start left without a result", seed, step);
      expect(children == 0 || children == 1, "more than one helper child", seed, step);
      expect(children >= 0, "a result consumed a child that did not run", seed, step);
      expect((children == 1) == (phase == UpdatePhase::Checking), "a child runs outside Checking", seed, step);
      expect(swaps <= 1, "more than one swap helper", seed, step);
      expect(swaps == 0 || verified, "a swap without a verified stage", seed, step);
      expect(from != UpdatePhase::HandedOff || next.action == UpdateAction::None, "an action after HandedOff", seed,
             step);
      expect(childBefore || (event != UpdateEvent::CheckedReady && event != UpdateEvent::CheckedNone)
                 || next.action == UpdateAction::None,
             "a result acted with no child", seed, step);
      expect(phase != UpdatePhase::Ready || verified, "Restart shows without a verified stage", seed, step);
      expect(!swapFailed || phase == UpdatePhase::Ready, "a failed swap start lost the ready stage", seed, step);
      expect(swaps == 0 || phase == UpdatePhase::HandedOff, "the swap helper runs and the app keeps the stage", seed,
             step);
      expect(event != UpdateEvent::Quit || phase == UpdatePhase::HandedOff || (swapFailed && phase == UpdatePhase::Ready),
             "quit did not hand off", seed, step);
      expect(event != UpdateEvent::Quit || from != UpdatePhase::Checking || next.action == UpdateAction::StopCheck,
             "quit left the helper child running", seed, step);
    }
  }
  return g_failures == 0 ? 0 : 1;
}
