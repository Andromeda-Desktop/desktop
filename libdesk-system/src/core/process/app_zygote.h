#pragma once

#include <sys/types.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// App zygote (andromeda-appd): preloads libdesk, fonts and Mesa once and forks one child per launch. The child loads
// the app library and runs its main, so a launch skips exec, library load and font setup.
namespace process::zygote {

  struct Request {
    std::vector<std::string> argv;
    std::string workingDir;
    std::string activationToken;
    // Set by launch(); the zygote refuses the request after it, so a client that stopped waiting never gets a child.
    std::chrono::steady_clock::time_point deadline;
  };

  // Empty when XDG_RUNTIME_DIR is unset.
  [[nodiscard]] std::filesystem::path socketPath(std::string_view appId);

  [[nodiscard]] std::string encode(const Request& request);
  [[nodiscard]] std::optional<Request> decode(std::span<const char> message);

  // Asks the zygote of appId to start a child. Returns its pid, or nothing when no zygote answers within 1 s.
  [[nodiscard]] std::optional<pid_t> launch(std::string_view appId, const Request& request);

  // Runs preload, then serves launches on socketPath(appId) forever; a request accept() refuses gets -1, so the
  // caller execs. Returns only in a forked child, with the child's request applied (working dir, activation token,
  // new session). preload must start no thread except the Pango fontconfig worker, which each child starts again.
  [[nodiscard]] Request serve(
      std::string_view appId, const std::function<void()>& preload, const std::function<bool(const Request&)>& accept
  );

} // namespace process::zygote
