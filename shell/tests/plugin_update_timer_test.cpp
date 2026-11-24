// Model test for the plugin update clock: random Start/Stop/Show/Hide sequences against an independent model of the
// two facts (started, shown). Invariants after every event: the mode encodes exactly those facts; the timer is armed
// if and only if the mode is Running; an update runs at once only when a paused script becomes shown.

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

  PluginUpdateMode modeOf(bool started, bool shown) {
    if (started) {
      return shown ? PluginUpdateMode::Running : PluginUpdateMode::Paused;
    }
    return shown ? PluginUpdateMode::StoppedShown : PluginUpdateMode::StoppedHidden;
  }
} // namespace

int main() {
  constexpr std::array kEvents{
      PluginUpdateEvent::Start, PluginUpdateEvent::Stop, PluginUpdateEvent::Show, PluginUpdateEvent::Hide
  };
  for (unsigned seed = 1; seed <= 64; ++seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::size_t> pick(0, kEvents.size() - 1);
    bool started = false;
    bool shown = true;
    bool armed = false;
    PluginUpdateMode mode = PluginUpdateMode::StoppedShown;
    for (int step = 0; step < 200; ++step) {
      const PluginUpdateEvent event = kEvents[pick(rng)];
      const bool wasPaused = started && !shown;
      started = event == PluginUpdateEvent::Start || (started && event != PluginUpdateEvent::Stop);
      shown = event == PluginUpdateEvent::Show || (shown && event != PluginUpdateEvent::Hide);

      const PluginUpdateStep next = nextPluginUpdate(mode, event);
      expect(!(next.arm && next.disarm), "a step both arms and disarms", seed, step);
      if (next.disarm) {
        armed = false;
      }
      if (next.arm) {
        expect(!armed, "armed an armed timer", seed, step);
        armed = true;
      }
      mode = next.next;
      expect(mode == modeOf(started, shown), "mode does not encode started x shown", seed, step);
      expect(armed == (mode == PluginUpdateMode::Running), "timer armed outside Running", seed, step);
      expect(next.updateNow == (wasPaused && started && shown), "catch-up update outside Paused -> Running", seed, step);
    }
  }
  static_assert(PluginUpdateTimer::clampInterval(0) == PluginUpdateTimer::kFloorMs);
  return gFailures == 0 ? 0 : 1;
}
