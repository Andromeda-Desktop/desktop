#include "shell_integration.h"
#include "core/sys/sys.h"

#include <filesystem>
#include <system_error>

namespace term {

  ShellLaunch shellLaunch(const std::string& shell, const std::string& dir, const EnvLookup& getenv) {
    const std::string name = std::filesystem::path(shell).filename().string();
    if (dir.empty()) {
      return {.argv = {shell, "-l"}, .env = {"VTE_VERSION=7600"}};
    }
    if (name == "bash") {
      return {.argv = {shell, "--posix", "-i"},
              .env = {"ENV=" + dir + "/bash/andromeda.bash", "ANDROMEDA_BASH_INJECT=login"}};
    }
    if (name == "zsh") {
      ShellLaunch launch{.argv = {shell, "-l"}, .env = {"ZDOTDIR=" + dir + "/zsh"}};
      if (const char* zdotdir = getenv("ZDOTDIR"); zdotdir != nullptr) {
        launch.env.push_back(std::string("ANDROMEDA_ZSH_ZDOTDIR=") + zdotdir);
      }
      return launch;
    }
    if (name == "fish") {
      const char* dataDirs = getenv("XDG_DATA_DIRS");
      const std::string rest = dataDirs != nullptr && *dataDirs != '\0' ? dataDirs : "/usr/local/share:/usr/share";
      return {.argv = {shell, "-l"},
              .env = {"XDG_DATA_DIRS=" + dir + ":" + rest, "ANDROMEDA_FISH_DATA_DIR=" + dir}};
    }
    return {.argv = {shell, "-l"}, .env = {"VTE_VERSION=7600"}};
  }

  std::string shellIntegrationDir() {
    std::error_code ec;
    const std::filesystem::path exe = sys::executablePath();
    if (!exe.empty()) {
      const auto prefix = exe.parent_path().parent_path();
      for (const auto& installed : {prefix / "share/andromeda/terminal/shell-integration", prefix / "Resources/shell-integration"}) {
        if (std::filesystem::is_directory(installed, ec)) {
          return installed.string();
        }
      }
    }
#ifdef ANDROMEDA_TERMINAL_SOURCE_SHELL_DIR
    if (std::filesystem::is_directory(ANDROMEDA_TERMINAL_SOURCE_SHELL_DIR, ec)) {
      return ANDROMEDA_TERMINAL_SOURCE_SHELL_DIR;
    }
#endif
    return {};
  }

} // namespace term
