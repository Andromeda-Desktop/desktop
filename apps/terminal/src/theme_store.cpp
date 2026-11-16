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

    // scene.glsl or one background picture; an error message when the folder breaks the rules.
    std::optional<std::string> readBackdrop(const std::filesystem::path& folder, ThemePackage& package) {
      constexpr std::uintmax_t kSceneLimit = 64 * 1024;
      constexpr std::uintmax_t kPictureLimit = 64 * 1024 * 1024;
      std::error_code ec;
      std::vector<std::filesystem::path> pictures;
      for (const char* ext : {".png", ".jpg", ".jpeg", ".webp", ".jxl", ".avif"}) {
        if (std::filesystem::is_regular_file(folder / ("background" + std::string(ext)), ec)) {
          pictures.push_back(folder / ("background" + std::string(ext)));
        }
      }
      const std::uintmax_t sceneSize = std::filesystem::file_size(folder / "scene.glsl", ec);
      const bool hasScene = !ec;
      if (pictures.size() + (hasScene ? 1 : 0) > 1) {
        return "use scene.glsl or one background picture, not both";
      }
      if (hasScene) {
        if (sceneSize > kSceneLimit) {
          return "scene.glsl is larger than 64 KiB";
        }
        std::ifstream in(folder / "scene.glsl", std::ios::binary);
        std::ostringstream bytes;
        bytes << in.rdbuf();
        package.backdrop = ThemePackage::Shader{std::move(bytes).str()};
      } else if (!pictures.empty()) {
        if (std::filesystem::file_size(pictures[0], ec) > kPictureLimit || ec) {
          return pictures[0].filename().string() + " is larger than 64 MiB";
        }
        const auto stamp = std::filesystem::last_write_time(pictures[0], ec).time_since_epoch().count();
        package.backdrop = ThemePackage::Picture{pictures[0].string(), static_cast<std::int64_t>(stamp)};
      }
      return std::nullopt;
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
        } else if (auto package = std::get<ThemePackage>(std::move(parsed)); auto backdropError = readBackdrop(folder, package)) {
          entry.state = invalid(std::move(*backdropError));
        } else {
          entry.state = ThemeEntry::Valid{std::move(package)};
        }
      }
      return entry;
    }

    // A hidden folder belongs to a tool working in the themes folder (a theme agent's .claude), not to a theme.
    bool isPackageFolder(const std::filesystem::directory_entry& item, std::error_code& ec) {
      return item.is_directory(ec) && !item.path().filename().string().starts_with('.');
    }

  } // namespace

  std::vector<ThemeEntry> scanThemes(const std::filesystem::path& dir, const std::vector<ThemeEntry>& previous) {
    std::vector<ThemeEntry> entries;
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(dir, ec)) {
      if (isPackageFolder(item, ec)) {
        entries.push_back(readEntry(item.path(), previous));
      }
    }
    std::ranges::sort(entries, {}, &ThemeEntry::id);
    return entries;
  }

  std::filesystem::path themesDir() { return std::filesystem::path(g_get_user_data_dir()) / "andromeda" / "themes"; }

  ThemeEntry readThemeFolder(const std::filesystem::path& folder) { return readEntry(folder, {}); }

  std::variant<std::string, SettingsError> importThemeFolder(const std::filesystem::path& source,
                                                             const std::filesystem::path& dir) {
    std::string base;
    for (const char c : source.filename().string()) {
      const char lower = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
      const bool alnum = (lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9');
      if (alnum || ((lower == '_' || lower == '-') && !base.empty())) {
        base += lower;
      } else if (!base.empty() && base.back() != '-') {
        base += '-';
      }
    }
    base = base.substr(0, 60);
    while (!base.empty() && (base.back() == '-' || base.back() == '_')) {
      base.pop_back();
    }
    if (base.empty()) {
      base = "theme";
    }
    std::error_code ec;
    std::string id = base;
    for (int n = 2; builtinTheme(id) || std::filesystem::exists(dir / id, ec); ++n) {
      id = base + "-" + std::to_string(n);
    }

    // Staged beside the themes folder, not in it: the store lists every folder in it.
    std::filesystem::create_directories(dir, ec);
    std::string pattern = (dir.parent_path() / ".theme-import-XXXXXX").string();
    if (g_mkdtemp(pattern.data()) == nullptr) {
      return SettingsError{"cannot create a folder next to " + dir.string()};
    }
    const std::filesystem::path staging(pattern);
    const std::filesystem::path package = staging / id;
    std::filesystem::create_directory(package, ec);
    for (const char* name : {"theme.toml", "scene.glsl", "background.png", "background.jpg", "background.jpeg",
                             "background.webp", "background.jxl", "background.avif"}) {
      if (std::filesystem::is_regular_file(source / name, ec)) {
        std::filesystem::copy_file(source / name, package / name, ec);
        if (ec) {
          std::filesystem::remove_all(staging, ec);
          return SettingsError{std::string("cannot copy ") + name};
        }
      }
    }
    std::variant<std::string, SettingsError> result = id;
    if (auto entry = readThemeFolder(package); auto* invalid = std::get_if<ThemeEntry::Invalid>(&entry.state)) {
      result = invalid->error;
    } else {
      std::filesystem::rename(package, dir / id, ec);
      if (ec) {
        result = SettingsError{"cannot move the theme into " + dir.string() + ": " + ec.message()};
      }
    }
    std::filesystem::remove_all(staging, ec);
    return result;
  }

  // The user's packages, then the valid packages the system ships (<XDG_DATA_DIRS>/andromeda/terminal/themes) under an
  // id the user has not taken: a user copy of a shipped theme replaces it.
  std::vector<ThemeEntry> ThemeStore::scan(const std::vector<ThemeEntry>& previous) const {
    std::vector<ThemeEntry> entries = scanThemes(m_dir, previous);
    for (const gchar* const* dir = g_get_system_data_dirs(); *dir != nullptr; ++dir) {
      for (ThemeEntry& shipped : scanThemes(std::filesystem::path(*dir) / "andromeda" / "terminal" / "themes", previous)) {
        if (std::holds_alternative<ThemeEntry::Valid>(shipped.state) &&
            std::ranges::none_of(entries, [&](const ThemeEntry& e) { return e.id == shipped.id; })) {
          entries.push_back(std::move(shipped));
        }
      }
    }
    std::ranges::sort(entries, {}, &ThemeEntry::id);
    return entries;
  }

  ThemeStore::ThemeStore() : m_dir(themesDir()) {
    std::error_code ec;
    std::filesystem::create_directories(m_dir, ec);
    m_entries = scan({});
    watch();
  }

  const ThemePackage* ThemeStore::find(std::string_view id) const { return lastGoodOf(m_entries, id); }

  void ThemeStore::reload() {
    watch();
    std::vector<ThemeEntry> entries = scan(m_entries);
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
      if (isPackageFolder(item, ec)) {
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
