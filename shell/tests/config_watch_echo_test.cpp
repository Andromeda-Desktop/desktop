// The overrides watcher skips only the echo of the shell's own write. An external edit that lands in
// the same inotify batch as that echo, or after an echo that never arrived, must still reload.

#include "config/config_service.h"
#include "protocols/async_file_writer.h"
#include "core/timer_manager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <poll.h>
#include <print>
#include <string>
#include <unistd.h>

namespace {

  int g_failures = 0;

  void expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "config_watch_echo: FAIL: {}", message);
      ++g_failures;
    }
  }

  void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << content;
  }

  // One rename event, as editors save: a truncate-then-write edit spreads over batches.
  void writeFileAtomic(const std::filesystem::path& path, std::string_view content) {
    const auto temp = path.string() + ".tmp";
    writeFile(temp, content);
    std::filesystem::rename(temp, path);
  }

  std::filesystem::path findOverrides(const std::filesystem::path& stateRoot) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(stateRoot)) {
      if (entry.path().filename() == "overrides.toml") {
        return entry.path();
      }
    }
    return {};
  }

  bool waitFor(DirectoryWatcher& dirs, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      pollfd descriptor{.fd = dirs.fd(), .events = POLLIN};
      if (::poll(&descriptor, 1, 10) > 0) {
        dirs.processEvents();
      }
      TimerManager::instance().tick();
      if (done()) {
        return true;
      }
    }
    return false;
  }

  bool enabled(const ConfigService& config, std::string_view id) {
    return std::ranges::contains(config.config().plugins.enabled, id);
  }

  // Reads the queued events without delivering them: the config never sees them.
  void drainWatch(DirectoryWatcher& dirs) {
    std::array<char, 4096> buffer{};
    while (true) {
      pollfd descriptor{.fd = dirs.fd(), .events = POLLIN};
      if (::poll(&descriptor, 1, 50) <= 0 || ::read(dirs.fd(), buffer.data(), buffer.size()) <= 0) {
        return;
      }
    }
  }

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("noctalia-watch-echo-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  writeFile(root / "config" / "andromeda" / "shell" / "config.toml", "[shell]\n");
  ::setenv("ANDROMEDA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);

  DirectoryWatcher dirs;
  ConfigService config(&dirs);
  expect(dirs.fd() >= 0, "inotify watch is set up");

  // Setters write on the background writer: the file lands after they return. The wait leaves the echo queued.
  std::filesystem::path overrides;
  const auto landed = [&] {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      overrides = findOverrides(root / "state");
      if (!overrides.empty() && !andromeda::pendingFileText(overrides).has_value()) {
        return true;
      }
      ::poll(nullptr, 0, 5);
    }
    return false;
  };

  // External edit in the same batch as the echo of a setter write.
  config.setPluginEnabled("me/own", true);
  expect(landed(), "setter wrote overrides.toml");
  writeFileAtomic(overrides, "[plugins]\nenabled = [\"ext/same-batch\"]\n");
  expect(waitFor(dirs, [&] { return enabled(config, "ext/same-batch"); }), "same-batch external edit reloads");

  // External edit after a setter write whose echo never reaches the watcher.
  config.setPluginEnabled("me/second", true);
  expect(landed(), "second setter wrote overrides.toml");
  drainWatch(dirs);
  writeFileAtomic(overrides, "[plugins]\nenabled = [\"ext/after-lost-echo\"]\n");
  expect(
      waitFor(dirs, [&] { return enabled(config, "ext/after-lost-echo"); }), "external edit after a lost echo reloads"
  );

  // External edit back to the exact text of the shell's last write, after another edit was loaded.
  config.setOverride({"wallpaper", "fill_mode"}, std::string("stretch"));
  const auto fill = [&] { return config.config().wallpaper.fillMode; };
  std::string ownWrite;
  waitFor(dirs, [&] {
    std::ifstream in(overrides);
    ownWrite.assign(std::istreambuf_iterator<char>(in), {});
    return ownWrite.contains("stretch");
  });
  waitFor(dirs, [] { return false; });
  std::string edited = ownWrite;
  edited.replace(edited.find("stretch"), 7, "center");
  writeFileAtomic(overrides, edited);
  expect(waitFor(dirs, [&] { return fill() == WallpaperFillMode::Center; }), "external fill edit reloads");
  writeFileAtomic(overrides, ownWrite);
  expect(
      waitFor(dirs, [&] { return fill() == WallpaperFillMode::Stretch; }),
      "external edit restoring the last written text reloads"
  );

  std::filesystem::remove_all(root);
  return g_failures == 0 ? 0 : 1;
}
