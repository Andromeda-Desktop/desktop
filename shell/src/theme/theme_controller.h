#pragma once

#include "theme/theme_resolve.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Owner of the shown desktop theme: the selected id, the last good resolved table and its generation. A theme or an
// overrides file that fails to load never replaces what is shown; it keeps its last good copy, and the other file's
// edits still show over it (theme-engine STORES.md, invariants I1 and I5).
namespace noctalia::theme {

  class ThemeController {
  public:
    struct Disk {
      std::function<std::expected<ThemePackage, ThemeIssue>(std::string_view id)> loadTheme;
      std::function<std::expected<std::optional<ThemePackage>, ThemeIssue>()> loadOverrides;
    };
    // Shows `table` as `generation`; a table without a chain is the overrides over the built-in tokens. Returns the
    // reason when nothing changed; the controller then keeps its last good table and generation.
    using Apply = std::function<std::expected<void, std::string>(const ResolvedTheme& table, std::uint64_t generation)>;

    ThemeController(Disk disk, ThemeVocabulary vocabulary, BuiltInNumber builtIn, Apply apply);

    void themeFilesChanged();
    void overridesChanged();
    // The config's theme id (empty: the built-in tokens, with the overrides over them) and accent source in one
    // resolve; `overridesEdited` rereads theme-overrides.toml first. The overrides' "accent" colour roles apply only
    // with `userAccent` (theme.accent_source custom).
    void update(std::string_view id, bool userAccent, bool overridesEdited);

    [[nodiscard]] const std::string& selected() const noexcept { return m_selected; }
    // Id of the shown theme; empty while no theme shows (the overrides over the built-in tokens).
    [[nodiscard]] std::string_view shownId() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }
    // Ids whose theme.toml the caller watches: the shown chain, and the selected id while it fails to load.
    [[nodiscard]] std::vector<std::string> watchedIds() const;
    // The shown theme's accent for `scheme` ("dark" or "light") without the user's; nullopt: the built-in token.
    [[nodiscard]] std::optional<Color> themeAccent(std::string_view scheme) const;
    // theme-overrides.toml's accent for `scheme`, kept while another accent source shows.
    [[nodiscard]] std::optional<Color> userAccent(std::string_view scheme) const;
    // Why the selected theme or the overrides file is not shown as is; empty when both loaded and showed.
    [[nodiscard]] std::vector<ThemeIssue> errors() const;

  private:
    bool loadOverrides();
    void refresh();
    void show(ResolvedTheme table, std::map<std::string, ThemeValue> themeAccents);

    Disk m_disk;
    ThemeVocabulary m_vocabulary;
    BuiltInNumber m_builtIn;
    Apply m_apply;
    std::string m_selected;
    ResolvedTheme m_shown; // no chain: the overrides over the built-in tokens
    std::map<std::string, ThemeValue> m_themeAccents; // the shown theme's scheme.<scheme>.accent, without the user's
    std::optional<ThemePackage> m_overrides; // last good copy of theme-overrides.toml
    std::map<std::string, ThemePackage, std::less<>> m_lastGood; // last good package of each id in the shown chain
    bool m_userAccent = false;
    std::optional<ThemeIssue> m_themeError;
    std::optional<ThemeIssue> m_overridesError;
    std::optional<ThemeIssue> m_applyError;
    std::uint64_t m_generation = 0;
  };

} // namespace noctalia::theme
