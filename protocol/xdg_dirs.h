#pragma once

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>
#ifndef _WIN32
#include <pwd.h>
#include <unistd.h>
#endif
#include <vector>

// XDG Base Directory Specification 0.8: the one reader of HOME and the XDG_* directory variables. An unset, empty
// or relative value is ignored, as the spec says. Read on each call: tests and the session change them.
namespace xdg {

  namespace detail {

    [[nodiscard]] inline bool absolute(const char* value) { return value != nullptr && value[0] == '/'; }

    [[nodiscard]] inline std::string fromEnv(const char* name) {
      const char* value = std::getenv(name);
      return absolute(value) ? std::string(value) : std::string();
    }

    [[nodiscard]] inline std::vector<std::string> list(const char* name, std::string_view fallback) {
      const char* value = std::getenv(name);
      const std::string_view text = value != nullptr && value[0] != '\0' ? std::string_view(value) : fallback;
      std::vector<std::string> dirs;
      for (std::size_t start = 0; start <= text.size();) {
        const std::size_t end = std::min(text.find(':', start), text.size());
        if (const std::string_view dir = text.substr(start, end - start); dir.starts_with('/')) {
          dirs.emplace_back(dir);
        }
        start = end + 1;
      }
      return dirs;
    }

  } // namespace detail

  // $HOME, else the passwd entry; empty when neither is absolute (starts with /), so always empty on Windows.
  [[nodiscard]] inline std::string home() {
    if (std::string dir = detail::fromEnv("HOME"); !dir.empty()) {
      return dir;
    }
#ifdef _WIN32
    return {};
#else
    const passwd* entry = getpwuid(getuid());
    return entry != nullptr && detail::absolute(entry->pw_dir) ? std::string(entry->pw_dir) : std::string();
#endif
  }

  namespace detail {

    [[nodiscard]] inline std::string userDir(const char* name, std::string_view underHome) {
      if (std::string dir = fromEnv(name); !dir.empty()) {
        return dir;
      }
      const std::string base = home();
      return base.empty() ? std::string() : base + "/" + std::string(underHome);
    }

  } // namespace detail

  [[nodiscard]] inline std::string configHome() { return detail::userDir("XDG_CONFIG_HOME", ".config"); }
  [[nodiscard]] inline std::string dataHome() { return detail::userDir("XDG_DATA_HOME", ".local/share"); }
  [[nodiscard]] inline std::string stateHome() { return detail::userDir("XDG_STATE_HOME", ".local/state"); }
  [[nodiscard]] inline std::string cacheHome() { return detail::userDir("XDG_CACHE_HOME", ".cache"); }
  // No fallback: empty when unset or relative.
  [[nodiscard]] inline std::string runtimeDir() { return detail::fromEnv("XDG_RUNTIME_DIR"); }
  [[nodiscard]] inline std::vector<std::string> dataDirs() {
    return detail::list("XDG_DATA_DIRS", "/usr/local/share:/usr/share");
  }
  [[nodiscard]] inline std::vector<std::string> configDirs() { return detail::list("XDG_CONFIG_DIRS", "/etc/xdg"); }

} // namespace xdg
