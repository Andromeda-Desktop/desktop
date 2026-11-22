#pragma once

#include <functional>
#include <string>
#include <vector>

namespace term {

  // How to start a shell so it loads the terminal's own integration (OSC 133 marks with the command line, OSC 7 cwd)
  // without touching the user's rc files: bash through POSIX-mode $ENV, zsh through ZDOTDIR, fish through
  // XDG_DATA_DIRS vendor_conf.d. Other shells start as login shells with VTE_VERSION (the distro's vte.sh).
  struct ShellLaunch {
    std::vector<std::string> argv;
    std::vector<std::string> env; // NAME=value, added to the terminal's base environment
  };

  using EnvLookup = std::function<const char*(const char*)>;

  // `dir` holds bash/, zsh/ and fish/ (data/shell-integration); empty: no integration.
  [[nodiscard]] ShellLaunch shellLaunch(const std::string& shell, const std::string& dir, const EnvLookup& getenv);

  // The installed data dir next to the executable, else the source tree (a build-dir run), else empty.
  [[nodiscard]] std::string shellIntegrationDir();

} // namespace term
