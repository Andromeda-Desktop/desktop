#include "theme_store.h"

#include "core/log.h"

#include <gio/gio.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace term {

  namespace {

    constexpr Logger kLog("themes");

    const ThemePackage* lastGoodOf(const std::vector<ThemeEntry>& entries, std::string_view id) {
      for (const ThemeEntry& e : entries) {
        if (e.id == id) {
          if (const auto* valid = std::get_if<ThemeEntry::Valid>(&e.state)) {
            return &valid->package;
          }
          const auto& invalid = std::get<ThemeEntry::Invalid>(e.state);
          return invalid.lastGood ? &*invalid.lastGood : nullptr;
        }
      }
      return nullptr;
    }

    // A missing scene.glsl is no scene; false when the file is too large to be a scene.
    bool readScene(const std::filesystem::path& folder, std::string& out) {
      constexpr std::uintmax_t kSceneLimit = 64 * 1024;
      std::error_code ec;
      const std::uintmax_t size = std::filesystem::file_size(folder / "scene.glsl", ec);
      if (ec) {
        return true;
      }
      if (size > kSceneLimit) {
        return false;
      }
      std::ifstream in(folder / "scene.glsl", std::ios::binary);
      std::ostringstream bytes;
      bytes << in.rdbuf();
      out = std::move(bytes).str();
      return true;
    }

    ThemeEntry readEntry(const std::filesystem::path& folder, const std::vector<ThemeEntry>& previous) {
      ThemeEntry entry{.id = folder.filename().string(), .state = {}};
      const ThemePackage* lastGood = lastGoodOf(previous, entry.id);
      auto invalid = [&](std::string message, int line = 0) {
        return ThemeEntry::Invalid{SettingsError{std::move(message), line},
                                   lastGood != nullptr ? std::optional(*lastGood) : std::nullopt};
      };
      if (!isThemeId(entry.id)) {
        entry.state = invalid("the folder name must use only a-z, 0-9, - and _");
      } else if (builtinTheme(entry.id)) {
        entry.state = invalid("\"" + entry.id + "\" is a built-in theme name");
      } else if (std::ifstream in(folder / "theme.toml", std::ios::binary); !in) {
        entry.state = invalid("theme.toml is missing");
      } else {
        std::ostringstream bytes;
        bytes << in.rdbuf();
        auto parsed = parseThemePackage(bytes.str());
        if (auto* error = std::get_if<SettingsError>(&parsed)) {
          entry.state = invalid(std::move(error->message), error->line);
        } else if (auto package = std::get<ThemePackage>(std::move(parsed)); !readScene(folder, package.scene)) {
          entry.state = invalid("scene.glsl is larger than 64 KiB");
        } else {
          entry.state = ThemeEntry::Valid{std::move(package)};
        }
      }
      return entry;
    }

  } // namespace

  std::vector<ThemeEntry> scanThemes(const std::filesystem::path& dir, const std::vector<ThemeEntry>& previous) {
    std::vector<ThemeEntry> entries;
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(dir, ec)) {
      if (item.is_directory(ec)) {
        entries.push_back(readEntry(item.path(), previous));
      }
    }
    std::ranges::sort(entries, {}, &ThemeEntry::id);
    return entries;
  }

  ThemeStore::ThemeStore() : m_dir(std::filesystem::path(g_get_user_data_dir()) / "andromeda" / "themes") {
    std::error_code ec;
    std::filesystem::create_directories(m_dir, ec);
    m_entries = scanThemes(m_dir, {});
    watch();
  }

  const ThemePackage* ThemeStore::find(std::string_view id) const { return lastGoodOf(m_entries, id); }

  void ThemeStore::reload() {
    watch();
    std::vector<ThemeEntry> entries = scanThemes(m_dir, m_entries);
    if (entries == m_entries) {
      return;
    }
    for (const ThemeEntry& e : entries) {
      if (const auto* invalid = std::get_if<ThemeEntry::Invalid>(&e.state)) {
        kLog.warn("{}/{}/theme.toml:{}: {}", m_dir.string(), e.id, invalid->error.line, invalid->error.message);
      }
    }
    m_entries = std::move(entries);
    m_changed.emit();
  }

  // One monitor on the themes folder and one per package folder: GIO monitors are not recursive.
  void ThemeStore::watch() {
    std::vector<std::filesystem::path> folders{m_dir};
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(m_dir, ec)) {
      if (item.is_directory(ec)) {
        folders.push_back(item.path());
      }
    }
    std::erase_if(m_monitors, [&](const Watch& w) {
      if (std::ranges::find(folders, w.folder) != folders.end()) {
        return false;
      }
      g_file_monitor_cancel(w.monitor);
      g_signal_handlers_disconnect_by_data(w.monitor, this);
      // The monitor may be the one emitting this event: drop it after the emission.
      g_idle_add_once([](gpointer monitor) { g_object_unref(monitor); }, w.monitor);
      return true;
    });
    for (const auto& folder : folders) {
      if (std::ranges::find(m_monitors, folder, &Watch::folder) != m_monitors.end()) {
        continue;
      }
      GFile* file = g_file_new_for_path(folder.c_str());
      GFileMonitor* monitor = g_file_monitor_directory(file, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
      g_object_unref(file);
      if (monitor == nullptr) {
        kLog.warn("cannot watch {}", folder.string());
        continue;
      }
      g_signal_connect(monitor, "changed",
                       G_CALLBACK(+[](GFileMonitor*, GFile*, GFile*, GFileMonitorEvent event, gpointer self) {
                         // A plain write sends CHANGED per chunk, then CHANGES_DONE_HINT: read once, at the end.
                         if (event != G_FILE_MONITOR_EVENT_CHANGED && event != G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED) {
                           static_cast<ThemeStore*>(self)->reload();
                         }
                       }),
                       this);
      m_monitors.push_back({folder, monitor});
    }
  }

  ThemeStore::~ThemeStore() {
    for (const Watch& w : m_monitors) {
      g_file_monitor_cancel(w.monitor);
      g_signal_handlers_disconnect_by_data(w.monitor, this);
      g_object_unref(w.monitor);
    }
  }

} // namespace term
