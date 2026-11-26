#pragma once

#include "protocols/toml.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// The [lockscreen] table of the greeter's sync.toml: one parser for the apply helper and the greeter launcher.
namespace greeter::config {

  struct GreeterLockscreen {
    std::string theme;
    std::string layout;
    std::string clockStyle;
    std::string clockSize; // "" | s | m | l | xl | fill
    bool clockFormat24h = true;
    bool clockSeconds = false;
    // Font axes; 0 (round -1) = the style's own value.
    int clockWeight = 0;  // 0 | 100..1000
    int clockWidth = 0;   // 0 | 50..200
    int clockRound = -1;  // -1..100
    int clockSlant = 0;   // 0..15 degrees
    std::string clockMaterial = "solid"; // solid | glass | tint
    std::string clockColor = "white";    // white | accent | wallpaper | gradient
    bool clockMotion = true;
    // The lock clock's gradient ink, "#rrggbb"; empty = not synced.
    std::string clockInkFrom;
    std::string clockInkTo;
    double blurIntensity = 0.5; // 0..1
    double tintIntensity = 0.3; // 0..1
  };
  [[nodiscard]] bool isSafeLockscreenId(std::string_view id);
  [[nodiscard]] bool isValidLockscreen(const GreeterLockscreen& lockscreen);

  // A table with a wrong type or out-of-range value is dropped whole (nullopt), so the greeter keeps its defaults.
  [[nodiscard]] std::optional<GreeterLockscreen> parseLockscreen(const toml::table& root, const std::filesystem::path& path);

} // namespace greeter::config
