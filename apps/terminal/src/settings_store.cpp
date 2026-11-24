#include "settings_store.h"

#include "core/log.h"
#include "protocols/xdg_dirs.h"

#include <gio/gio.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <pwd.h>
#include <unistd.h>

namespace term {

  namespace {

    constexpr Logger kLog("settings");

    std::filesystem::path configPath() {
      const std::string config = xdg::configHome();
      const std::filesystem::path base = config.empty() ? std::filesystem::path("/tmp") : std::filesystem::path(config);
      return base / "andromeda" / "terminal.toml";
    }

  } // namespace

  std::string defaultShell() {
    if (const char* shell = std::getenv("SHELL"); shell != nullptr && *shell != '\0') {
      return shell;
    }
    if (const passwd* pw = getpwuid(getuid()); pw != nullptr && pw->pw_shell != nullptr) {
      return pw->pw_shell;
    }
    return "/bin/sh";
  }

  SettingsStore::SettingsStore() : m_path(configPath()) {
    std::error_code ec;
    std::filesystem::create_directories(m_path.parent_path(), ec);
    load();
    GFile* file = g_file_new_for_path(m_path.c_str());
    GError* error = nullptr;
    m_monitor = g_file_monitor_file(file, G_FILE_MONITOR_WATCH_MOVES, nullptr, &error);
    g_object_unref(file);
    if (m_monitor == nullptr) {
      kLog.warn("cannot watch {}: {}", m_path.string(), error != nullptr ? error->message : "?");
      g_clear_error(&error);
      return;
    }
    g_signal_connect(m_monitor, "changed",
                     G_CALLBACK(+[](GFileMonitor*, GFile*, GFile*, GFileMonitorEvent event, gpointer self) {
                       // A plain write sends CHANGED per chunk, then CHANGES_DONE_HINT: read once, at the end.
                       if (event != G_FILE_MONITOR_EVENT_CHANGED && event != G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED) {
                         static_cast<SettingsStore*>(self)->load();
                       }
                     }),
                     this);
  }

  SettingsStore::~SettingsStore() {
    if (m_monitor != nullptr) {
      g_file_monitor_cancel(m_monitor);
      g_signal_handlers_disconnect_by_data(m_monitor, this);
      g_object_unref(m_monitor);
    }
  }

  void SettingsStore::load() {
    std::ifstream in(m_path, std::ios::binary);
    if (!in) {
      if (!std::holds_alternative<Missing>(m_state) || !m_bytes.empty()) {
        m_bytes.clear();
        set(Settings{}, Missing{});
      }
      return;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::string bytes = std::move(buffer).str();
    if (bytes == m_bytes && !std::holds_alternative<Missing>(m_state)) {
      return;
    }
    m_bytes = std::move(bytes);
    auto parsed = parseSettings(m_bytes);
    if (auto* error = std::get_if<SettingsError>(&parsed)) {
      kLog.warn("{}:{}: {}", m_path.string(), error->line, error->message);
      set(m_settings, Invalid{std::move(*error)});
      return;
    }
    set(std::get<Settings>(std::move(parsed)), Loaded{});
  }

  void SettingsStore::update(const Settings& settings) {
    std::string bytes = writeSettings(settings, m_bytes);
    const std::filesystem::path temp = m_path.string() + ".new";
    {
      std::ofstream out(temp, std::ios::binary | std::ios::trunc);
      out << bytes;
      if (!out.flush()) {
        kLog.warn("cannot write {}", temp.string());
        return;
      }
    }
    std::error_code ec;
    std::filesystem::rename(temp, m_path, ec);
    if (ec) {
      kLog.warn("cannot replace {}: {}", m_path.string(), ec.message());
      return;
    }
    m_bytes = std::move(bytes);
    set(settings, Loaded{});
  }

  void SettingsStore::set(Settings settings, FileState state) {
    m_settings = std::move(settings);
    m_state = std::move(state);
    m_changed.emit();
  }

} // namespace term
