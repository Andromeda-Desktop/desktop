#include "shell/bar/widgets/battery_tone.h"

#include <cstring>
#include <print>

// Walks every level, state, plug and saver combination and checks the tone's precedence.
int main() {
  bool ok = true;
  const BatteryState states[] = {BatteryState::Unknown,      BatteryState::Charging,      BatteryState::Discharging,
                                 BatteryState::FullyCharged, BatteryState::PendingCharge, BatteryState::Empty};
  for (const int threshold : {0, 5, 20, 30}) {
    for (const BatteryState state : states) {
      for (int pct = 0; pct <= 100; ++pct) {
        for (const bool plugged : {false, true}) {
          for (const bool saver : {false, true}) {
            const BatteryTone tone = batteryTone(state, pct, plugged, threshold, saver);
            BatteryTone want = BatteryTone::Normal;
            if (state == BatteryState::Unknown) {
              want = BatteryTone::Unknown;
            } else if (state == BatteryState::Charging) {
              want = BatteryTone::Charging;
            } else if (plugged) {
              want = BatteryTone::Plugged;
            } else if (pct <= kBatteryCriticalPercent) {
              want = BatteryTone::Critical;
            } else if (pct <= kBatteryLowPercent || pct <= threshold) {
              want = BatteryTone::Low;
            } else if (saver) {
              want = BatteryTone::Saver;
            }
            if (tone != want) {
              std::println(stderr, "battery_tone_test: threshold {} pct {} plugged {} saver {}: wrong tone", threshold,
                           pct, plugged, saver);
              ok = false;
            }
          }
        }
      }
    }
  }
  const auto glyph = [](BatteryTone tone, BatteryState state) {
    const char* g = batteryToneGlyph(tone, state);
    return g != nullptr ? g : "";
  };
  if (std::strcmp(glyph(BatteryTone::Critical, BatteryState::Discharging), "exclamation-mark") != 0
      || std::strcmp(glyph(BatteryTone::Unknown, BatteryState::Unknown), "question-mark") != 0
      || std::strcmp(glyph(BatteryTone::Charging, BatteryState::Charging), "bolt-filled") != 0
      || std::strcmp(glyph(BatteryTone::Plugged, BatteryState::FullyCharged), "plug-filled") != 0
      || std::strcmp(glyph(BatteryTone::Low, BatteryState::Discharging), "") != 0) {
    std::println(stderr, "battery_tone_test: wrong overlay glyph");
    ok = false;
  }
  return ok ? 0 : 1;
}
