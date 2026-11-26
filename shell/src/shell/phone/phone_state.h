#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

class LinkPhoneState;
class LinkService;
struct LinkDevice;
struct LinkStatus;

// What the bar item, its popover, the quick settings tile, and the lock screen say about a Link phone.
namespace phone {

  // The phone the shell shows: the first connected one, else the first paired one.
  [[nodiscard]] const LinkDevice* primary(const LinkPhoneState& link);
  // Wi-Fi or Bluetooth while connected, else when the phone was last seen.
  [[nodiscard]] std::string connectionText(const LinkService& link, const LinkDevice& device);
  // "42%", or "42%, charging".
  [[nodiscard]] std::string batteryText(const LinkStatus& status);
  [[nodiscard]] std::string glyph(const LinkDevice& device, const LinkStatus* status);
  // The set-ringer value that follows `ringer` in the cycle normal, vibrate, silent.
  [[nodiscard]] std::uint32_t nextRinger(std::string_view ringer);
  [[nodiscard]] std::string ringerGlyph(std::string_view ringer);
  // The MPRIS bus name linkd gives a phone's player (link-daemon mpris.rs NAME_PREFIX).
  [[nodiscard]] std::string playerBusName(std::string_view deviceId);
  // The phone id in a phone player's bus name; nullopt for any other player.
  [[nodiscard]] std::optional<std::string_view> playerDeviceId(std::string_view busName);

  // One of a phone's Wi-Fi, Bluetooth and torch keys, as the phone panel and the control center draw it.
  struct ToggleKey {
    const char* command = "";
    // Translation keys of its short name, and of its tooltip.
    const char* label = "";
    const char* tooltip = "";
    std::string_view glyph;
    // A pending command shows the state it asks for, and takes no second press.
    bool on = false;
    bool enabled = false;
  };
  // Null for a disconnected phone or one without the toggle commands. The radio Link runs on stays enabled: linkd
  // refuses turning it off, and the key's tooltip says why.
  [[nodiscard]] std::optional<std::array<ToggleKey, 3>> toggleKeys(const LinkService& link, const std::string& deviceId);

} // namespace phone
