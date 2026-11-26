#include "config/config.h"

#include "core/files/directory_watcher.h"
#include "core/files/file_watcher.h"
#include "core/poll_loop.h"
#include "core/timer_manager.h"
#include "protocols/toml.h"
#include "protocols/xdg_dirs.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string_view>
#include <utility>

namespace xdpu {

  namespace {

    constexpr int kDebounceMs = 50;

    std::filesystem::path configPath() {
      return std::filesystem::path(xdg::configHome()) / "xdg-desktop-portal-andromeda" / "config.toml";
    }

    void readString(const toml::table& table, std::string_view tableName, std::string_view key, std::string& target) {
      if (const auto value = table[tableName][key].value<std::string>()) {
        target = *value;
      }
    }

    void readInt(const toml::table& table, std::string_view tableName, std::string_view key, int& target) {
      if (const auto value = table[tableName][key].value<int64_t>()) {
        if (*value >= std::numeric_limits<int>::min() && *value <= std::numeric_limits<int>::max()) {
          target = static_cast<int>(*value);
        }
      }
    }

  } // namespace

  Config loadConfig() {
    Config config;
    const auto path = configPath();

    std::error_code existsError;
    if (!std::filesystem::exists(path, existsError)) {
      return config;
    }

    try {
      const toml::table table = toml::parse_file(path.string());

      readString(table, "screencast", "chooser_cmd", config.screencast.chooserCmd);
      readInt(table, "screencast", "max_fps", config.screencast.maxFps);

      readString(table, "screenshot", "cmd", config.screenshot.cmd);
      readString(table, "screenshot", "color_pick_cmd", config.screenshot.colorPickCmd);

    } catch (const toml::parse_error& error) {
      const std::string description(error.description());
      std::fprintf(stderr, "config: unable to parse %s: %s\n", path.c_str(), description.c_str());
    }

    return config;
  }

  struct ConfigWatcher::Impl {
    Impl(PollLoop& loop, std::function<void(const Config& oldCfg, const Config& newCfg)> onChange)
        : loop(loop), onChange(std::move(onChange)), current(loadConfig()) {
      loop.addSource(&dirs);
      watch = files.watch(configPath(), [this] { scheduleReload(); });
      if (watch < 0) {
        std::fprintf(stderr, "config: cannot watch %s; hot reload disabled\n", configPath().c_str());
      }
    }

    ~Impl() {
      reloadTimer.stop();
      files.unwatch(watch);
      loop.removeSource(&dirs);
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void scheduleReload() {
      reloadTimer.start(std::chrono::milliseconds(kDebounceMs), [this] { reload(); });
    }

    void reload() {
      const Config next = loadConfig();
      if (next == current) {
        return;
      }

      const Config previous = current;
      current = next;
      if (onChange) {
        onChange(previous, current);
      }
    }

    PollLoop& loop;
    std::function<void(const Config& oldCfg, const Config& newCfg)> onChange;
    Config current;
    DirectoryWatcher dirs;
    FileWatcher files{dirs};
    FileWatcher::WatchId watch = -1;
    Timer reloadTimer;
  };

  ConfigWatcher::ConfigWatcher(PollLoop& loop, std::function<void(const Config& oldCfg, const Config& newCfg)> onChange)
      : m_impl(std::make_unique<Impl>(loop, std::move(onChange))) {}

  ConfigWatcher::~ConfigWatcher() = default;

} // namespace xdpu
