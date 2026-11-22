#include "terminal.h"

#include "core/log.h"

#include <glib.h>

namespace {
  constexpr Logger kLog("terminal");
} // namespace

std::string openTerminalIn(const std::filesystem::path& dir) {
  gchar* found = g_find_program_in_path("andromeda-terminal");
  if (found == nullptr) {
    kLog.warn("andromeda-terminal is not on PATH");
    return {};
  }
  std::string command = found;
  g_free(found);
  // The folder goes as an argument: a launch forwarded to the running instance keeps no working directory.
  gchar* argv[] = {command.data(), const_cast<gchar*>(dir.c_str()), nullptr};
  GError* error = nullptr;
  if (!g_spawn_async(dir.c_str(), argv, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, nullptr, &error)) {
    kLog.warn("{}: {}", command, error != nullptr ? error->message : "?");
    g_clear_error(&error);
    return {};
  }
  kLog.info("terminal {} in {}", command, dir.string());
  return command;
}
