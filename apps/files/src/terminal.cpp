#include "terminal.h"

#include "core/log.h"

#include <array>
#include <cstdlib>
#include <glib.h>
#include <vector>

namespace {
  constexpr Logger kLog("terminal");
  // Wayland-first terminals, then desktop ones; each opens in the spawn's working directory.
  constexpr std::array kKnown{"foot", "kitty", "ghostty", "alacritty", "wezterm", "ptyxis", "gnome-terminal", "konsole",
                              "xfce4-terminal", "xterm"};

  std::string onPath(const char* name) {
    gchar* found = g_find_program_in_path(name);
    std::string out = found != nullptr ? found : "";
    g_free(found);
    return out;
  }
} // namespace

std::string openTerminalIn(const std::filesystem::path& dir) {
  // $TERMINAL is the user's own choice and wins over the desktop default.
  std::string command;
  if (const char* env = std::getenv("TERMINAL"); env != nullptr && *env != '\0') {
    command = onPath(env);
  }
  // The desktop's own terminal takes the folder as an argument: a launch forwarded to its running instance keeps no
  // working directory.
  const bool own = command.empty() && !(command = onPath("andromeda-terminal")).empty();
  if (command.empty()) {
    command = onPath("xdg-terminal-exec");
  }
  for (const char* name : kKnown) {
    if (!command.empty()) {
      break;
    }
    command = onPath(name);
  }
  if (command.empty()) {
    kLog.warn("no terminal found");
    return {};
  }
  std::vector<gchar*> argv{const_cast<gchar*>(command.c_str())};
  if (own) {
    argv.push_back(const_cast<gchar*>(dir.c_str()));
  }
  argv.push_back(nullptr);
  GError* error = nullptr;
  if (!g_spawn_async(dir.c_str(), argv.data(), nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, nullptr, &error)) {
    kLog.warn("{}: {}", command, error != nullptr ? error->message : "?");
    g_clear_error(&error);
    return {};
  }
  kLog.info("terminal {} in {}", command, dir.string());
  return command;
}
