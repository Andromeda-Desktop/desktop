#pragma once

#include "config/schema/diagnostics.h"
#include "protocols/toml.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noctalia::config {

  inline constexpr std::string_view kConfigVersionKey = "config_version";

  struct ConfigMigration {
    int toVersion;
    std::string_view summary;
    void (*apply)(toml::table& root, schema::Diagnostics& diag);
  };

  struct LegacyConfigIssue {
    int migrationVersion;
    std::string path;
    std::string message;
    schema::SourceOrigin origin;

    bool operator==(const LegacyConfigIssue&) const = default;
  };

  using LegacyConfigIssues = std::vector<LegacyConfigIssue>;

  [[nodiscard]] const std::vector<ConfigMigration>& configMigrations();
  [[nodiscard]] int currentConfigVersion();
  [[nodiscard]] std::optional<int> storedConfigVersion(const toml::table& root, schema::Diagnostics& diag);
  [[nodiscard]] int applyPendingConfigMigrations(
      toml::table& root, int storedVersion, schema::Diagnostics& diag,
      std::span<const ConfigMigration> migrations = configMigrations()
  );

  void normalizeLegacyConfig(toml::table& root, LegacyConfigIssues& issues);

  // [shell.animation] moved to the compositor's animation.enabled and animation.speed. Returns the compositor settings
  // (key, value) for the values in `root` that differ from the old shell defaults.
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> legacyShellAnimationSettings(const toml::table& root);
  // Removes [shell.animation]; true when `root` had it.
  bool eraseLegacyShellAnimation(toml::table& root);
  // Config keys whose value lives in theme-overrides.toml [values] `themeKey`.
  struct LegacyThemeKey {
    std::string_view section;
    std::string_view key;
    std::string_view themeKey;
  };
  inline constexpr std::array kLegacyThemeKeys{
      LegacyThemeKey{.section = "shell", .key = "font_family", .themeKey = "font.ui"},
      LegacyThemeKey{.section = "dock", .key = "icon_size", .themeKey = "dock.icon"},
      LegacyThemeKey{.section = "dock", .key = "radius", .themeKey = "dock.corner"},
  };
  // Compositor settings whose value lives in theme-overrides.toml [values]. The old value applied in both schemes, so
  // it moves to each of `themeKeys`; a saved value in `shellWritten` came from an earlier shell and is only cleared.
  struct LegacyCompositorThemeKey {
    std::string_view compositorKey;
    std::span<const std::string_view> themeKeys;
    std::span<const double> shellWritten;
  };
  inline constexpr std::array<std::string_view, 1> kWindowCornerThemeKeys{"corner.window"};
  // Earlier builds saved radius 8 for the Windows Desktop Layout.
  inline constexpr std::array kWindowCornerShellWritten{8.0};
  inline constexpr std::array<std::string_view, 2> kBlurSaturationThemeKeys{"glass.saturation_dark", "glass.saturation_light"};
  inline constexpr std::array kLegacyCompositorThemeKeys{
      LegacyCompositorThemeKey{
          .compositorKey = "appearance.corner_radius",
          .themeKeys = kWindowCornerThemeKeys,
          .shellWritten = kWindowCornerShellWritten,
      },
      LegacyCompositorThemeKey{.compositorKey = "appearance.blur.saturation", .themeKeys = kBlurSaturationThemeKeys},
  };
  // The compositor setting that held the user's accent; it moves to theme-overrides.toml [values.dark|light] accent.
  inline constexpr std::string_view kLegacyCompositorAccentKey = "appearance.accent";
  // Removes each kLegacyThemeKeys entry from `root`; returns the ones it held.
  std::vector<LegacyThemeKey> eraseLegacyThemeKeys(toml::table& root);

  [[nodiscard]] std::string legacyConfigIssueFingerprint(const LegacyConfigIssues& issues);
  [[nodiscard]] bool
  legacyConfigFingerprintHasNewIssues(std::string_view currentFingerprint, std::string_view previousFingerprint);
  [[nodiscard]] bool
  legacyConfigReminderIntervalElapsed(std::int64_t nowEpochSeconds, std::int64_t previousEpochSeconds);

  inline constexpr std::int64_t kLegacyConfigReminderIntervalSeconds = 3 * 24 * 60 * 60;

} // namespace noctalia::config
