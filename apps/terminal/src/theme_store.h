#pragma once

#include "settings.h"
#include "ui/signal.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

typedef struct _GFileMonitor GFileMonitor;

namespace term {

  // One folder under the themes folder. A bad edit keeps the last good package in use and reports the error.
  struct ThemeEntry {
    struct Valid {
      ThemePackage package;
      bool operator==(const Valid&) const = default;
    };
    struct Invalid {
      SettingsError error;
      std::optional<ThemePackage> lastGood;
      bool operator==(const Invalid&) const = default;
    };
    std::string id;
    std::variant<Valid, Invalid> state;
    bool operator==(const ThemeEntry&) const = default;
  };

  // <XDG_DATA_HOME>/andromeda/themes
  [[nodiscard]] std::filesystem::path themesDir();
  // One package folder, as the store reads it.
  [[nodiscard]] ThemeEntry readThemeFolder(const std::filesystem::path& folder);
  // Copies the package files of `source` (any folder name) into a new <dir>/<id>, only when they form a valid package.
  // The id comes from the folder name, made unique; it appears in `dir` in one rename, so a watcher sees it complete.
  [[nodiscard]] std::variant<std::string, SettingsError> importThemeFolder(const std::filesystem::path& source,
                                                                         const std::filesystem::path& dir);

  // Reads every <dir>/<id>/theme.toml, sorted by id; previous supplies the last good package of each id.
  [[nodiscard]] std::vector<ThemeEntry> scanThemes(const std::filesystem::path& dir,
                                                   const std::vector<ThemeEntry>& previous);

  // The installed theme packages, kept current while the folder or a theme.toml changes.
  class ThemeStore {
  public:
    ThemeStore();
    ~ThemeStore();
    ThemeStore(const ThemeStore&) = delete;
    ThemeStore& operator=(const ThemeStore&) = delete;

    [[nodiscard]] const std::vector<ThemeEntry>& entries() const noexcept { return m_entries; }
    // The package in use for id: the valid one, else the last good one.
    [[nodiscard]] const ThemePackage* find(std::string_view id) const;
    [[nodiscard]] const std::filesystem::path& dir() const noexcept { return m_dir; }
    [[nodiscard]] Signal<>& changed() noexcept { return m_changed; }

  private:
    void reload();
    void watch();

    struct Watch {
      std::filesystem::path folder;
      GFileMonitor* monitor;
    };

    std::filesystem::path m_dir;
    std::vector<ThemeEntry> m_entries;
    std::vector<Watch> m_monitors;
    Signal<> m_changed;
  };

} // namespace term
