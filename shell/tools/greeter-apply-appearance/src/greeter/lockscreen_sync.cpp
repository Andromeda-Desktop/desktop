#include "greeter/lockscreen_sync.h"

#include "core/log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace greeter::config {

  namespace {
    constexpr Logger kLog("greeter-config");
  } // namespace

  bool isSafeLockscreenId(std::string_view id) {
    return id.size() <= 32 && std::ranges::all_of(id, [](char c) {
             return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
           });
  }

  bool isValidLockscreen(const GreeterLockscreen& lockscreen) {
    const auto unit = [](double v) { return std::isfinite(v) && v >= 0.0 && v <= 1.0; };
    const auto ink = [](std::string_view v) {
      return v.empty() || (v.size() == 7 && v[0] == '#' && std::ranges::all_of(v.substr(1), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
      }));
    };
    return isSafeLockscreenId(lockscreen.theme)
        && isSafeLockscreenId(lockscreen.layout)
        && isSafeLockscreenId(lockscreen.clockStyle)
        && (lockscreen.clockSize.empty()
            || lockscreen.clockSize == "s"
            || lockscreen.clockSize == "m"
            || lockscreen.clockSize == "l"
            || lockscreen.clockSize == "xl"
            || lockscreen.clockSize == "fill")
        && (lockscreen.clockWeight == 0 || (lockscreen.clockWeight >= 100 && lockscreen.clockWeight <= 1000))
        && (lockscreen.clockWidth == 0 || (lockscreen.clockWidth >= 50 && lockscreen.clockWidth <= 200))
        && lockscreen.clockRound >= -1 && lockscreen.clockRound <= 100
        && lockscreen.clockSlant >= 0 && lockscreen.clockSlant <= 15
        && (lockscreen.clockMaterial == "solid" || lockscreen.clockMaterial == "glass"
            || lockscreen.clockMaterial == "tint")
        && (lockscreen.clockColor == "white" || lockscreen.clockColor == "accent"
            || lockscreen.clockColor == "wallpaper" || lockscreen.clockColor == "gradient")
        && ink(lockscreen.clockInkFrom)
        && ink(lockscreen.clockInkTo)
        && unit(lockscreen.blurIntensity)
        && unit(lockscreen.tintIntensity);
  }

  std::optional<GreeterLockscreen> parseLockscreen(const toml::table& root, const std::filesystem::path& path) {
    const toml::table* table = root["lockscreen"].as_table();
    if (table == nullptr) {
      return std::nullopt;
    }
    GreeterLockscreen lockscreen;
    bool ok = true;
    const auto readString = [&](std::string_view key, std::string& out) {
      if (const toml::node* node = table->get(key)) {
        const auto value = node->value<std::string>();
        ok = ok && value.has_value();
        out = value.value_or("");
      }
    };
    const auto readBool = [&](std::string_view key, bool& out) {
      if (const toml::node* node = table->get(key)) {
        const auto value = node->value<bool>();
        ok = ok && value.has_value();
        out = value.value_or(out);
      }
    };
    const auto readInt = [&](std::string_view key, int& out) {
      if (const toml::node* node = table->get(key)) {
        const auto value = node->value<std::int64_t>();
        ok = ok && value.has_value() && *value >= -1000 && *value <= 1000;
        out = static_cast<int>(value.value_or(out));
      }
    };
    const auto readUnit = [&](std::string_view key, double& out) {
      if (const toml::node* node = table->get(key)) {
        const auto value = node->value<double>();
        ok = ok && value.has_value();
        out = value.value_or(out);
      }
    };
    readString("theme", lockscreen.theme);
    readString("layout", lockscreen.layout);
    readString("clock_style", lockscreen.clockStyle);
    readString("clock_size", lockscreen.clockSize);
    readBool("clock_format_24h", lockscreen.clockFormat24h);
    readBool("clock_seconds", lockscreen.clockSeconds);
    readInt("clock_weight", lockscreen.clockWeight);
    readInt("clock_width", lockscreen.clockWidth);
    readInt("clock_round", lockscreen.clockRound);
    readInt("clock_slant", lockscreen.clockSlant);
    readString("clock_material", lockscreen.clockMaterial);
    readString("clock_color", lockscreen.clockColor);
    readBool("clock_motion", lockscreen.clockMotion);
    readString("clock_ink_from", lockscreen.clockInkFrom);
    readString("clock_ink_to", lockscreen.clockInkTo);
    readUnit("blur_intensity", lockscreen.blurIntensity);
    readUnit("tint_intensity", lockscreen.tintIntensity);
    if (!ok || !isValidLockscreen(lockscreen)) {
      kLog.warn("{}: invalid [lockscreen] table (ignored)", path.string());
      return std::nullopt;
    }
    return lockscreen;
  }

} // namespace greeter::config
