#pragma once

#include "dbus/network/network_display.h"
#include "dbus/network/network_lock_client.h"
#include "dbus/upower/upower_service.h"
#include "shell/auth/auth_column.h"

#include <cmath>
#include <format>

// The battery and network part of the auth column status, shared by the lock screen and greeter mode.
[[nodiscard]] inline AuthColumn::Status authStatusFrom(const UPowerService* upower, const NetworkLockClient* network) {
  AuthColumn::Status status;
  if (upower != nullptr && upower->state().isPresent) {
    const auto& battery = upower->state();
    status.batteryGlyph = batteryGlyphName(battery.percentage, battery.state);
    status.batteryText = std::format("{}%", static_cast<int>(std::lround(battery.percentage)));
  }
  if (network != nullptr && network->hasState()) {
    status.networkGlyph = network_display::glyphForState(network->state());
  }
  return status;
}
