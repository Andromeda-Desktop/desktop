#pragma once

#include "dbus/upower/upower_service.h"

#include <algorithm>
#include <cstdint>

// What a battery shows beyond its level: one tone picks the colour (icon and percent label) and the overlay glyph.
enum class BatteryTone : std::uint8_t { Normal, Plugged, Charging, Low, Critical, Saver, Unknown };

inline constexpr int kBatteryLowPercent = 20;
inline constexpr int kBatteryCriticalPercent = 10;

// lowThreshold is the user's warning level: it raises the low level above kBatteryLowPercent, never lowers it.
[[nodiscard]] constexpr BatteryTone
batteryTone(BatteryState state, int percent, bool pluggedIn, int lowThreshold, bool powerSaver) noexcept {
  if (state == BatteryState::Unknown) {
    return BatteryTone::Unknown;
  }
  if (state == BatteryState::Charging) {
    return BatteryTone::Charging;
  }
  if (pluggedIn) {
    return BatteryTone::Plugged;
  }
  if (percent <= kBatteryCriticalPercent) {
    return BatteryTone::Critical;
  }
  if (percent <= std::max(lowThreshold, kBatteryLowPercent)) {
    return BatteryTone::Low;
  }
  return powerSaver ? BatteryTone::Saver : BatteryTone::Normal;
}

// Overlay glyph inside the battery body; nullptr for none.
[[nodiscard]] constexpr const char* batteryToneGlyph(BatteryTone tone, BatteryState state) noexcept {
  switch (tone) {
  case BatteryTone::Charging:
    return "bolt-filled";
  case BatteryTone::Plugged:
    return state == BatteryState::FullyCharged || state == BatteryState::PendingCharge ? "plug-filled" : nullptr;
  case BatteryTone::Critical:
    return "exclamation-mark";
  case BatteryTone::Unknown:
    return "question-mark";
  default:
    return nullptr;
  }
}
