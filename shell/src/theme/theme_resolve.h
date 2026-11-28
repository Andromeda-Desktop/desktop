#pragma once

#include "theme/theme_package.h"
#include "ui/token_slots.h"

#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// One theme resolved to a flat table (docs/theme-format.md section 3, without the accessibility layer: consumers
// apply ui_scale, text scale, reduce motion and high contrast at use).
namespace noctalia::theme {

  inline constexpr int kMaxExtendsDepth = 4;

  // The store's view of the theme roots: a parsed package by id, or why it cannot load.
  using ThemeLookup = std::function<std::expected<ThemePackage, ThemeIssue>(std::string_view id)>;
  // Built-in value of a number vocabulary key, used when a reference names a key no layer sets.
  using BuiltInNumber = std::function<std::optional<double>(std::string_view key)>;

  struct ResolvedTheme {
    std::vector<std::string> chain;           // the theme first, then each ancestor
    std::vector<std::string> schemes;         // the theme's own; empty without a theme (built-in: both)
    std::map<std::string, ThemeValue> values; // concrete values only: references are resolved, none remain
    std::map<std::string, std::string> origin; // key -> theme id or "overrides" that set the value (asset paths)
    std::vector<ThemeIssue> issues;           // each layer's report and each fallback of the resolution
  };

  // Fails only when `id` itself cannot load; the caller keeps its last good table. `overrides` (the user's
  // theme-overrides.toml) sits above the theme. An empty `id` resolves `overrides` over the built-in tokens.
  [[nodiscard]] std::expected<ResolvedTheme, ThemeIssue>
  resolveTheme(std::string_view id, const ThemeLookup& lookup, const ThemePackage* overrides,
               const ThemeVocabulary& vocabulary, const BuiltInNumber& builtIn);

  // The token slot values a resolved table sets (desk::tokens::applySlotValues input).
  [[nodiscard]] desk::tokens::SlotValues slotValues(const ResolvedTheme& theme, const ThemeVocabulary& vocabulary);

} // namespace noctalia::theme
