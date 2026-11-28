#pragma once

#include "theme/dev_tokens.h"
#include "theme/theme_controller.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class FileWatcher;

namespace noctalia::theme {

  // The one writer of the shell's token slots: the dev token file (dev builds) under the active desktop theme
  // (theme.desktop_theme), written in one applySlotValues per change.
  class DesktopTheme {
  public:
    // Writes the dev token file (ANDROMEDA_DEV_TOKENS) and theme-overrides.toml over the built-in tokens, and watches
    // both. Config defaults read tokens, so construct it before the config loads.
    explicit DesktopTheme(FileWatcher& watcher);
    ~DesktopTheme();

    DesktopTheme(const DesktopTheme&) = delete;
    DesktopTheme& operator=(const DesktopTheme&) = delete;

    // Shows the theme `id` (empty: the built-in tokens) under theme-overrides.toml with the caller's current
    // `overridesText` (a queued write first), so a write of that file applies before its watch fires. The overrides'
    // accent applies only with `userAccent` (theme.accent_source custom). Returns true when the token values changed.
    bool sync(std::string_view id, bool userAccent, const std::optional<std::string>& overridesText);

    // Runs after an edit of a watched file (the shown theme chain, theme-overrides.toml, the dev token file) changed
    // the token values.
    void setChangeCallback(std::function<void()> callback);

    // The shown theme's accent for the scheme, without the user's: what theme.accent_source theme shows.
    [[nodiscard]] Color themeAccent(bool light) const;
    // theme-overrides.toml's accent for the scheme: what theme.accent_source custom shows; nullopt when unset.
    [[nodiscard]] std::optional<Color> userAccent(bool light) const;

    // The scheme the shown theme pins ("dark" or "light"); nullopt when it has both or the built-in tokens show.
    [[nodiscard]] std::optional<std::string_view> pinnedScheme() const noexcept;

  private:
    // Writes the table's values; the shown values and scheme change only when the write succeeds.
    std::expected<void, std::string> show(const ResolvedTheme& table);
    // One applySlotValues of `dev` (when set) with `theme` over it.
    std::expected<void, std::string> write(const std::optional<dev_tokens::File>& dev,
                                           const desk::tokens::SlotValues& theme);
    void logErrors();
    void rewatchThemes();
    void watchedChange(const std::function<void()>& step);

    ThemeVocabulary m_vocabulary;
    std::optional<std::string> m_overridesText; // theme-overrides.toml as last loaded; nullopt: no file
    std::optional<dev_tokens::File> m_devFile; // last good: it parses and every expression resolves
    desk::tokens::SlotValues m_devBase;         // m_devFile resolved alone
    desk::tokens::SlotValues m_themeValues;
    std::vector<std::string> m_schemes;
    std::uint64_t m_written = 0; // count of slot writes that succeeded
    std::string m_loggedErrors;
    ThemeController m_controller;

    FileWatcher& m_watcher;
    std::function<void()> m_onChanged;
    std::vector<int> m_fileWatches;                         // theme-overrides.toml, dev token file
    std::map<std::string, std::vector<int>> m_themeWatches; // theme id -> watch ids (user and system theme.toml)
  };

} // namespace noctalia::theme
