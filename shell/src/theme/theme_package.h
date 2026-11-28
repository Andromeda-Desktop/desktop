#pragma once

#include "render/core/color.h"

#include <array>
#include <expected>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// A theme's theme.toml (docs/theme-format.md, format 1), checked against protocol/theme-vocabulary.toml.
namespace noctalia::theme {

  inline constexpr int kThemeFormat = 1;
  // Tables the engine reads by entry ("backdrop.desktop"); a layer that sets an entry replaces all of it.
  inline constexpr std::array<std::string_view, 3> kEngineTables = {"motion", "backdrop", "assets"};

  struct ThemeIssue {
    std::string key;
    std::string message;
    int line = 0;
    std::string source; // theme id or "overrides"; set by the resolver
  };

  enum class VocabularyType { Color, Length, Number, Font, Spring, Enum };

  struct VocabularyKey {
    VocabularyType type = VocabularyType::Number;
    double min = 0.0; // Length and Number only
    double max = 0.0;
    std::vector<std::string> choices; // Enum only
    std::vector<std::string> tokens;  // internal design tokens this key sets
  };

  struct ThemeVocabulary {
    int version = 0;
    std::map<std::string, VocabularyKey, std::less<>> keys;
    std::map<std::string, std::string, std::less<>> aliases; // old name -> current name
    std::vector<std::string> roles;                          // colour roles per scheme
  };

  [[nodiscard]] std::expected<ThemeVocabulary, ThemeIssue> parseThemeVocabulary(std::string_view tomlText);

  struct ThemeSpring {
    double response = 0.0; // seconds, > 0
    double bounce = 0.0;   // [0, 1)
    bool operator==(const ThemeSpring&) const = default;
  };

  // A "=..." expression over other vocabulary keys, resolved against the full value table.
  struct ThemeReference {
    std::string expression;
    int line = 0;
    bool operator==(const ThemeReference&) const = default;
  };

  // Strings that are not colours or references: font families, enum names, asset paths.
  using ThemeValue = std::variant<Color, double, bool, ThemeSpring, ThemeReference, std::string>;

  struct ThemeManifest {
    int format = 0;
    std::string id;
    std::string name;
    std::string version;
    std::string extends;
    std::vector<std::string> schemes;
    std::map<std::string, int> contracts;
  };

  struct ThemePackage {
    ThemeManifest manifest;
    // Vocabulary keys by current name ("corner.window"), colour roles as "scheme.<scheme>.<role>", and the engine
    // tables flattened ("motion.window_open.kind", "backdrop.desktop.scene"). An invalid key is absent, so it inherits.
    std::map<std::string, ThemeValue> values;
    // The theme report: fallbacks, ignored keys and renamed keys. The theme still loads.
    std::vector<ThemeIssue> issues;
  };

  // Fails only when the manifest itself is unusable (syntax, format, id, extends); a bad value is an issue.
  [[nodiscard]] std::expected<ThemePackage, ThemeIssue>
  parseThemePackage(std::string_view tomlText, std::string_view folderName, const ThemeVocabulary& vocabulary);

} // namespace noctalia::theme
