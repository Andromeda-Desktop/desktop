#pragma once

#include <optional>

namespace term::control {

  // The running app's remote-control object, on its single-instance bus name.
  inline constexpr const char* kService = "dev.andromeda.Terminal";
  inline constexpr const char* kPath = "/org/andromeda/Terminal";
  inline constexpr const char* kInterface = "org.andromeda.Terminal1";
  inline constexpr const char* kNoSession = "org.andromeda.Terminal1.Error.NoSession";
  inline constexpr const char* kNotTerminal = "org.andromeda.Terminal1.Error.NotTerminal";
  inline constexpr const char* kNoWindow = "org.andromeda.Terminal1.Error.NoWindow";
  inline constexpr const char* kInvalidArgs = "org.freedesktop.DBus.Error.InvalidArgs";

  // `andromeda-terminal ctl ...`: the exit code, or nullopt when argv[1] is not "ctl".
  std::optional<int> runCli(int argc, char* argv[]);

} // namespace term::control
