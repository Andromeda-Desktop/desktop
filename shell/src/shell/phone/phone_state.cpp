#include "shell/phone/phone_state.h"

#include "dbus/link/link_service.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "time/time_format.h"

#include <algorithm>
#include <chrono>
#include <format>

namespace phone {

  const LinkDevice* primary(const LinkPhoneState& link) {
    const auto& devices = link.devices();
    if (devices.empty()) {
      return nullptr;
    }
    const auto connected = std::ranges::find_if(devices, &LinkDevice::connected);
    return connected != devices.end() ? &*connected : &devices.front();
  }

  std::string connectionText(const LinkService& link, const LinkDevice& device) {
    const LinkDeviceInfo* info = link.info(device.id);
    if (device.connected) {
      if (info != nullptr && (info->transport == "wifi" || info->transport == "bluetooth")) {
        return i18n::tr("bar.widgets.phone.transport-" + info->transport);
      }
      return i18n::tr("bar.widgets.phone.connected");
    }
    if (info == nullptr || info->lastSeen <= 0) {
      return i18n::tr("bar.widgets.phone.not-connected");
    }
    const auto seen = std::chrono::system_clock::time_point(std::chrono::seconds(info->lastSeen));
    return i18n::tr("bar.widgets.phone.last-seen", "time", formatTimeAgo(seen));
  }

  std::string batteryText(const LinkStatus& status) {
    std::string text = std::format("{}%", status.battery);
    if (status.charging) {
      text += ", " + i18n::tr("bar.widgets.phone.charging");
    }
    return text;
  }

  std::string glyph(const LinkDevice& device, const LinkStatus* status) {
    if (status != nullptr) {
      return batteryGlyphName(status->battery, status->charging ? BatteryState::Charging : BatteryState::Discharging);
    }
    return device.connected ? "device-mobile" : "device-mobile-off";
  }

  std::uint32_t nextRinger(std::string_view ringer) {
    if (ringer == "normal") {
      return 1;
    }
    if (ringer == "vibrate") {
      return 0;
    }
    return 2;
  }

  std::string ringerGlyph(std::string_view ringer) {
    if (ringer == "vibrate") {
      return "device-mobile-vibration";
    }
    return ringer == "silent" ? "bell-off" : "bell";
  }

  std::optional<std::array<ToggleKey, 3>> toggleKeys(const LinkService& link, const std::string& deviceId) {
    const LinkToggles* toggles = link.toggles(deviceId);
    const LinkDeviceInfo* info = link.info(deviceId);
    if (toggles == nullptr) {
      return std::nullopt;
    }
    const LinkCommandState* command = link.commandState(deviceId);
    const std::string transport = info != nullptr ? info->transport : std::string();
    std::array<ToggleKey, 3> keys{{
        {.command = "set-wifi", .label = "bar.widgets.phone.wifi", .tooltip = "bar.widgets.phone.wifi"},
        {.command = "set-bluetooth", .label = "bar.widgets.phone.bluetooth", .tooltip = "bar.widgets.phone.bluetooth"},
        {.command = "set-torch", .label = "bar.widgets.phone.torch", .tooltip = "bar.widgets.phone.torch"},
    }};
    const std::array states{toggles->wifi, toggles->bluetooth, toggles->torch};
    const std::array<std::pair<std::string_view, std::string_view>, 3> glyphs{
        std::pair{"wifi", "wifi-off"}, std::pair{"bluetooth", "bluetooth-off"}, std::pair{"bulb", "bulb-off"}
    };
    const std::array<bool, 3> carries{transport == "wifi" || transport == "nearby", transport == "bluetooth", false};
    const std::array<const char*, 3> carriesTip{
        "bar.widgets.phone.wifi-carries-link", "bar.widgets.phone.bluetooth-carries-link", ""
    };
    for (std::size_t i = 0; i < keys.size(); ++i) {
      const bool pending = command != nullptr && command->phase == LinkCommandState::Phase::Pending &&
                           command->command == keys[i].command;
      keys[i].on = pending ? command->value == 1 : states[i] == 1;
      keys[i].enabled = states[i] >= 0 && !pending;
      keys[i].glyph = keys[i].on ? glyphs[i].first : glyphs[i].second;
      if (keys[i].on && carries[i]) {
        keys[i].tooltip = carriesTip[i];
      }
    }
    return keys;
  }

} // namespace phone
