#pragma once

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <stdexcept>
#include <string>

namespace term::control {

  // The running app's remote-control object, on its single-instance bus name (Linux; control_dbus.cpp). macOS
  // serves the same methods on a Unix socket (control_socket.cpp).
  inline constexpr const char* kService = "dev.andromeda.Terminal";
  inline constexpr const char* kPath = "/org/andromeda/Terminal";
  inline constexpr const char* kInterface = "org.andromeda.Terminal1";
  inline constexpr const char* kNoSession = "org.andromeda.Terminal1.Error.NoSession";
  inline constexpr const char* kNotTerminal = "org.andromeda.Terminal1.Error.NotTerminal";
  inline constexpr const char* kNoWindow = "org.andromeda.Terminal1.Error.NoWindow";
  inline constexpr const char* kInvalidArgs = "org.freedesktop.DBus.Error.InvalidArgs";
  inline constexpr const char* kUnknownMethod = "org.freedesktop.DBus.Error.UnknownMethod";
  inline constexpr const char* kNotRunning = "org.freedesktop.DBus.Error.ServiceUnknown";

  // A failed control call; name is one of the error names above.
  struct Error : std::runtime_error {
    Error(std::string errorName, const std::string& message) : std::runtime_error(message), name(std::move(errorName)) {}
    std::string name;
  };

  // The app's end of the transport.
  class Server {
  public:
    virtual ~Server() = default;
    // SessionsChanged or WorkspacesChanged.
    virtual void emitSignal(const char* name) = 0;
  };

  // Calls a method of the running app with the D-Bus method's arguments as a JSON array; throws Error.
  nlohmann::json call(const std::string& method, const nlohmann::json& args);

  // `andromeda-terminal ctl ...`: the exit code, or nullopt when argv[1] is not "ctl".
  std::optional<int> runCli(int argc, char* argv[]);

} // namespace term::control
