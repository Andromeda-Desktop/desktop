#pragma once

#include "theme/theme_resolve.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Owner of the shown desktop theme: the selected id, the last good resolved table and its generation. A theme or an
// overrides file that fails to load never replaces what is shown (theme-engine STORES.md, invariants I1 and I5).
namespace noctalia::theme {

  class ThemeController {
  public:
    struct Disk {
      std::function<std::expected<ThemePackage, ThemeIssue>(std::string_view id)> loadTheme;
      std::function<std::expected<std::optional<ThemePackage>, ThemeIssue>()> loadOverrides;
    };
    // Called once per new generation with the table to show; an empty table means the built-in tokens.
    using Apply = std::function<void(const ResolvedTheme& table, std::uint64_t generation)>;

    ThemeController(Disk disk, ThemeVocabulary vocabulary, BuiltInNumber builtIn, Apply apply);

    // The config's theme id; empty selects the built-in tokens.
    void select(std::string_view id);
    void themeFilesChanged();
    void overridesChanged();

    [[nodiscard]] const std::string& selected() const noexcept { return m_selected; }
    // Id of the shown theme; empty while the built-in tokens show.
    [[nodiscard]] std::string_view shownId() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }
    // Ids whose theme.toml the caller watches: the shown chain, and the selected id while it fails to load.
    [[nodiscard]] std::vector<std::string> watchedIds() const;
    // Why the selected theme or the overrides file is not shown as is; empty when both loaded.
    [[nodiscard]] std::vector<ThemeIssue> errors() const;

  private:
    struct BuiltIn {};
    struct Shown {
      ResolvedTheme table;
    };

    void refresh();
    void show(ResolvedTheme table);

    Disk m_disk;
    ThemeVocabulary m_vocabulary;
    BuiltInNumber m_builtIn;
    Apply m_apply;
    std::string m_selected;
    std::variant<BuiltIn, Shown> m_shown;
    std::optional<ThemePackage> m_overrides; // last good copy of theme-overrides.toml
    std::optional<ThemeIssue> m_themeError;
    std::optional<ThemeIssue> m_overridesError;
    std::uint64_t m_generation = 0;
  };

} // namespace noctalia::theme
