#pragma once

#include "protocols/ipc_channel.h"

#include <sys/types.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// App zygote (andromeda-appd): preloads libdesk, fonts and Mesa once and forks one child per launch. The child loads
// the app library and runs its main, so a launch skips exec, library load and font setup.
namespace process::zygote {

  struct Request {
    // Set by launch(): a status query names the launch by it.
    std::string id;
    std::vector<std::string> argv;
    std::string workingDir;
    std::string activationToken;
    // Set by launch(); the zygote refuses the request after it, so a client that stopped waiting never gets a child.
    std::chrono::steady_clock::time_point deadline;
  };

  // Empty when XDG_RUNTIME_DIR is unset.
  [[nodiscard]] std::filesystem::path socketPath(std::string_view appId);

  // Asks for the outcome of the launch with this id. Reply: the child pid, -1 refused, 0 unknown id.
  struct StatusQuery {
    std::string id;
  };
  using Message = std::variant<Request, StatusQuery>;

  [[nodiscard]] std::string encode(const Message& message);
  [[nodiscard]] std::optional<Message> decode(std::span<const char> message);

  // Refused: no child exists or will exist, so the caller may start the app itself. Unknown: a child may exist, so
  // the caller must not start a second one.
  struct LaunchResult {
    enum class Outcome { Started, Refused, Unknown };
    Outcome outcome = Outcome::Refused;
    pid_t pid = 0; // Started only
    // Started only; pins the child even after it exits. Empty from a zygote older than this field.
    ipc::Fd pidfd;
  };

  // Asks the zygote of appId to start a child. When the reply does not come within 1 s, asks for the launch's status
  // on a new connection: the zygote serves connections in order, so its answer is final.
  [[nodiscard]] LaunchResult launch(std::string_view appId, const Request& request);

  // Runs preload, then serves launches and status queries on socketPath(appId) forever; a request accept() refuses
  // gets -1, so the caller execs. Returns only in a forked child, with the child's request applied (working dir,
  // activation token, new session). preload must start no thread except the Pango fontconfig worker, which each
  // child starts again.
  [[nodiscard]] Request serve(
      std::string_view appId, const std::function<void()>& preload, const std::function<bool(const Request&)>& accept
  );

} // namespace process::zygote
