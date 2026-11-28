#pragma once

#include "theme/theme_package.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Desktop theme folders on disk (docs/theme-format.md sections 1 and 9) and the user's theme-overrides.toml.
namespace noctalia::theme {

  struct ThemeRoots {
    std::filesystem::path user;   // $XDG_DATA_HOME/andromeda/desktop-themes, searched first
    std::filesystem::path system; // /usr/share/andromeda/desktop-themes
  };

  struct ThemeFolderLimits {
    std::uintmax_t maxBytes = 64ULL * 1024 * 1024;
    std::size_t maxFiles = 4096;
  };

  // protocols/theme-vocabulary.toml as shipped with this build.
  [[nodiscard]] const ThemeVocabulary& shippedVocabulary();

  [[nodiscard]] ThemeRoots defaultThemeRoots();
  // $XDG_CONFIG_HOME/andromeda/theme-overrides.toml (ANDROMEDA_CONFIG_HOME replaces XDG_CONFIG_HOME).
  [[nodiscard]] std::filesystem::path themeOverridesPath();

  // Regular files and folders only; a symlink must resolve inside the folder; size and file count within limits.
  [[nodiscard]] std::expected<void, ThemeIssue> checkThemeFolder(const std::filesystem::path& folder,
                                                                 const ThemeFolderLimits& limits = {});

  // The theme from the first root that holds `id`: a broken user copy fails and does not fall back to the system one.
  [[nodiscard]] std::expected<ThemePackage, ThemeIssue> loadTheme(const ThemeRoots& roots, std::string_view id,
                                                                  const ThemeVocabulary& vocabulary,
                                                                  const ThemeFolderLimits& limits = {});

  // The text of theme-overrides.toml: a queued write first, else the disk. No file: nullopt.
  [[nodiscard]] std::expected<std::optional<std::string>, ThemeIssue>
  readThemeOverridesText(const std::filesystem::path& path);

  // No file: nullopt. A file that does not parse: the error (the caller keeps its last good overrides).
  [[nodiscard]] std::expected<std::optional<ThemePackage>, ThemeIssue>
  loadThemeOverrides(const std::filesystem::path& path, const ThemeVocabulary& vocabulary);
  [[nodiscard]] std::expected<std::optional<ThemePackage>, ThemeIssue>
  parseThemeOverridesText(const std::optional<std::string>& text, const ThemeVocabulary& vocabulary);

} // namespace noctalia::theme
