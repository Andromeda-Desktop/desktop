#include "render/text/glyph_registry.h"

#include "core/log.h"
#include "tabler_glyphs.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace {

  constexpr Logger kLog("glyph");
  constexpr char32_t kMissingGlyph = 0xF292; // skull

  // Hand-curated alias -> native Tabler icon name map.
  // Use these for semantic shell states and stable Noctalia-facing names.
  // clang-format off
const std::unordered_map<std::string, std::string_view> kAliases = {
    // General
    {"close", "x"},
    {"add", "plus"},
    {"more-vertical", "dots-vertical"},
    {"person", "user"},
    {"info", "file-description"},
    {"unpin", "pinned-off"},
    {"image", "photo"},
    {"capslock", "keyboard"},
    {"numlock", "keyboard"},
    {"scrolllock", "keyboard"},
    {"plugin", "plug-connected"},
    {"official-plugin", "shield-filled"},
    {"package", "box"},

    // Toast / warnings
    {"toast-notice", "circle-check"},
    {"toast-warning", "alert-circle"},
    {"toast-error", "circle-x"},
    {"warning", "exclamation-circle"},

    // Media
    {"media-pause", "player-pause-filled"},
    {"media-play", "player-play-filled"},
    {"media-prev", "player-skip-back-filled"},
    {"media-next", "player-skip-forward-filled"},
    {"shuffle", "arrows-shuffle"},
    {"stop", "player-stop-filled"},
    {"microphone-mute", "microphone-off"},

    // Volume
    {"volume-high", "volume"},
    {"volume-low", "volume-2"},
    {"volume-mute", "volume-off"},
    {"volume-x", "volume-3"},
    {"volume-zero", "volume-3"},

    // Network speed
    {"download-speed", "download"},
    {"upload-speed", "upload"},

    // System monitor
    {"cpu-intensive", "alert-octagon"},
    {"cpu-usage", "brand-speedtest"},
    {"cpu-temperature", "flame"},
    {"gpu-usage", "device-desktop"},
    {"memory", "cpu"},
    {"storage", "database"},
    {"busy", "hourglass-empty"},

    // Power
    {"performance", "gauge"},
    {"balanced", "scale"},
    {"powersaver", "leaf"},
    {"shutdown", "power"},
    {"reboot", "refresh"},
    {"reboot-uefi", "device-desktop-cog"},
    {"suspend", "player-pause"},
    {"hibernate", "zzz"},

    // Night light / dark mode
    {"nightlight-on", "sun-low"},
    {"nightlight-off", "moon-off"},
    {"nightlight-forced", "moon-stars"},
    {"theme-mode", "circle-half-2"},

    // Caffeine (idle inhibitor)
    {"caffeine-on", "coffee"},
    {"caffeine-off", "mug"},

    // Brightness / Display
    {"brightness-low", "brightness-down-filled"},
    {"brightness-high", "brightness-up-filled"},

    // Wallpaper / color
    {"wallpaper-selector", "library-photo"},

    // Battery
    {"battery-0", "battery"},
    {"battery-plugged", "battery-charging-2"},

    // Bluetooth devices
    {"bluetooth-device-generic", "bluetooth"},
    {"bluetooth-device-gamepad", "device-gamepad-2"},
    {"bluetooth-device-microphone", "microphone"},
    {"bluetooth-device-headset", "headset"},
    {"bluetooth-device-earbuds", "device-airpods"},
    {"bluetooth-device-headphones", "headphones"},
    {"bluetooth-device-mouse", "mouse-2"},
    {"bluetooth-device-keyboard", "keyboard"},
    {"bluetooth-device-phone", "device-mobile"},
    {"bluetooth-device-watch", "device-watch"},
    {"bluetooth-device-speaker", "device-speaker"},
    {"bluetooth-device-tv", "device-tv"},

    // Weather
    {"weather-sun", "sun"},
    {"weather-moon", "moon"},
    {"weather-moon-stars", "moon-stars"},
    {"weather-cloud", "cloud"},
    {"weather-cloud-off", "cloud-off"},
    {"weather-cloud-haze", "cloud-fog"},
    {"weather-cloud-lightning", "cloud-bolt"},
    {"weather-cloud-rain", "cloud-rain"},
    {"weather-cloud-snow", "cloud-snow"},
    {"weather-cloud-sun", "cloud-sun"},
    {"weather-sunrise", "sunrise"},
    {"weather-sunset", "sunset"},
};
  // clang-format on

  [[nodiscard]] std::optional<char32_t> parseCodepointLiteral(std::string_view value) {
    if (value.size() < 3) {
      return std::nullopt;
    }

    const bool unicodePrefix = (value[0] == 'U' || value[0] == 'u') && value[1] == '+';
    const bool hexPrefix = value[0] == '0' && (value[1] == 'x' || value[1] == 'X');
    if (!unicodePrefix && !hexPrefix) {
      return std::nullopt;
    }
    const std::string_view hex = value.substr(2);

    if (hex.empty()) {
      return std::nullopt;
    }

    std::uint32_t codepoint = 0;
    const auto* begin = hex.data();
    const auto* end = begin + hex.size();
    const auto result = std::from_chars(begin, end, codepoint, 16);
    if (result.ec != std::errc{} || result.ptr != end || codepoint == 0 || codepoint > 0x10FFFF) {
      return std::nullopt;
    }
    return static_cast<char32_t>(codepoint);
  }

  [[nodiscard]] GlyphRegistry::TablerGlyph glyphAt(std::size_t index) {
    const tabler_data::Entry& e = tabler_data::kEntries[index];
    return {
        .name = std::string_view(tabler_data::kNames + e.nameOffset, e.nameLength),
        .codepoint = static_cast<char32_t>(e.codepoint),
        .category = tabler_data::kCategories[e.category],
    };
  }

  [[nodiscard]] std::optional<GlyphRegistry::TablerGlyph> findTabler(std::string_view name) {
    const auto& entries = tabler_data::kEntries;
    const auto nameOf = [](const tabler_data::Entry& e) {
      return std::string_view(tabler_data::kNames + e.nameOffset, e.nameLength);
    };
    const auto it = std::ranges::lower_bound(entries, name, {}, nameOf);
    if (it == entries.end() || nameOf(*it) != name) {
      return std::nullopt;
    }
    return glyphAt(static_cast<std::size_t>(it - entries.begin()));
  }

  // An alias resolves to its Tabler target; any other name is a Tabler name itself.
  [[nodiscard]] std::string_view tablerName(std::string_view name) {
    const auto alias = kAliases.find(std::string(name));
    return alias != kAliases.end() ? alias->second : name;
  }

} // namespace

bool GlyphRegistry::contains(std::string_view name) {
  return parseCodepointLiteral(name).has_value() || findTabler(tablerName(name)).has_value();
}

char32_t GlyphRegistry::lookup(std::string_view name) {
  if (auto codepoint = parseCodepointLiteral(name)) {
    return *codepoint;
  }
  const std::string_view target = tablerName(name);
  if (const auto glyph = findTabler(target)) {
    return glyph->codepoint;
  }
  if (target != name) {
    kLog.warn("missing Tabler glyph '{}' for alias '{}'", target, name);
  } else {
    kLog.warn("missing glyph: {}", name);
  }
  return kMissingGlyph;
}

std::size_t GlyphRegistry::tablerGlyphCount() { return tabler_data::kEntries.size(); }

GlyphRegistry::TablerGlyph GlyphRegistry::tablerGlyphAt(std::size_t index) { return glyphAt(index); }

std::optional<GlyphRegistry::TablerGlyph> GlyphRegistry::findTablerGlyph(std::string_view name) {
  return findTabler(name);
}

std::optional<std::string_view> GlyphRegistry::categoryFor(std::string_view name) {
  if (const auto glyph = findTabler(tablerName(name))) {
    return glyph->category;
  }
  return std::nullopt;
}

const std::unordered_map<std::string, std::string_view>& GlyphRegistry::aliases() { return kAliases; }
