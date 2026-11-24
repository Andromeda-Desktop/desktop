// Model test for the plugin update clock: random Start/Stop/Show/Hide events and timer fires (with a random answer
// from the deferral) against an independent model of the facts started, shown and waiting (a due update waits for
// the deferral). Invariants after every event: the mode encodes exactly those facts; the interval timer is armed
// only in Running and the retry only in Deferred; an update runs only when it is due, started, shown and not
// deferred, and every such due runs exactly one.

#include "scripting/plugin_update_timer.h"

#include <array>
#include <print>
#include <random>

namespace {
  int gFailures = 0;

  void expect(bool condition, const char* message, unsigned seed, int step) {
    if (!condition) {
      std::println(stderr, "plugin_update_timer_test: seed {} step {}: {}", seed, step, message);
      ++gFailures;
    }
  }

  enum class Armed : std::uint8_t { None, Interval, Retry };

  PluginUpdateMode modeOf(bool started, bool shown, bool waiting) {
    if (started) {
      if (!shown) {
        return PluginUpdateMode::Paused;
      }
      return waiting ? PluginUpdateMode::Deferred : PluginUpdateMode::Running;
    }
    return shown ? PluginUpdateMode::StoppedShown : PluginUpdateMode::StoppedHidden;
  }

  enum class Input : std::uint8_t { Start, Stop, Show, Hide, Fire };
} // namespace

int main() {
  constexpr std::array kInputs{Input::Start, Input::Stop, Input::Show, Input::Hide, Input::Fire, Input::Fire};
  for (unsigned seed = 1; seed <= 64; ++seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::size_t> pick(0, kInputs.size() - 1);
    std::bernoulli_distribution deferNow(0.4);
    bool started = false;
    bool shown = true;
    bool waiting = false;
    Armed armed = Armed::None;
    PluginUpdateMode mode = PluginUpdateMode::StoppedShown;
    for (int step = 0; step < 200; ++step) {
      const Input input = kInputs[pick(rng)];
      if (input == Input::Fire && armed == Armed::None) {
        continue;
      }
      const bool defer = deferNow(rng);
      const bool wasPaused = started && !shown;

      // The model: which facts hold after the input, and whether an update is due now.
      bool dueNow = false;
      switch (input) {
      case Input::Start:
        started = true;
        break;
      case Input::Stop:
        started = false;
        waiting = false;
        break;
      case Input::Show:
        shown = true;
        dueNow = wasPaused && started;
        break;
      case Input::Hide:
        shown = false;
        waiting = false;
        break;
      case Input::Fire:
        dueNow = true;
        break;
      }
      if (dueNow) {
        waiting = defer;
      }
      const bool expectUpdate = dueNow && !defer;

      PluginUpdateEvent event = PluginUpdateEvent::Start;
      switch (input) {
      case Input::Start:
        event = PluginUpdateEvent::Start;
        break;
      case Input::Stop:
        event = PluginUpdateEvent::Stop;
        break;
      case Input::Show:
        event = PluginUpdateEvent::Show;
        break;
      case Input::Hide:
        event = PluginUpdateEvent::Hide;
        break;
      case Input::Fire:
        event = defer ? PluginUpdateEvent::DueDeferred : PluginUpdateEvent::Due;
        break;
      }

      // The owner's loop: a step that asks for `due` applies Due or DueDeferred at once.
      int updates = 0;
      bool more = true;
      while (more) {
        const PluginUpdateStep next = nextPluginUpdate(mode, event);
        expect(static_cast<int>(next.arm) + static_cast<int>(next.retry) + static_cast<int>(next.disarm) <= 1,
               "a step arms, retries or disarms more than one way", seed, step);
        if (next.disarm) {
          armed = Armed::None;
        }
        if (next.arm) {
          armed = Armed::Interval;
        }
        if (next.retry) {
          armed = Armed::Retry;
        }
        updates += next.update ? 1 : 0;
        mode = next.next;
        more = next.due;
        event = defer ? PluginUpdateEvent::DueDeferred : PluginUpdateEvent::Due;
      }

      expect(mode == modeOf(started, shown, waiting), "mode does not encode started x shown x waiting", seed, step);
      expect((armed == Armed::Interval) == (mode == PluginUpdateMode::Running), "interval armed outside Running", seed,
             step);
      expect((armed == Armed::Retry) == (mode == PluginUpdateMode::Deferred), "retry armed outside Deferred", seed,
             step);
      expect(updates == (expectUpdate ? 1 : 0), "update count differs from the due updates", seed, step);
    }
  }
  static_assert(PluginUpdateTimer::clampInterval(0) == PluginUpdateTimer::kFloorMs);
  static_assert(PluginUpdateTimer::firstDelay(1000, 0) == std::chrono::milliseconds(1000));
  static_assert(PluginUpdateTimer::firstDelay(1000, 3) == std::chrono::milliseconds(1150));
  static_assert(PluginUpdateTimer::firstDelay(16, 3) <= std::chrono::milliseconds(23));
  return gFailures == 0 ? 0 : 1;
}
