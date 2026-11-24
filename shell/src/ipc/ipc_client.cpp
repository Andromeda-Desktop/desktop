#include "ipc/ipc_client.h"

#include "protocols/ipc_channel.h"
#include "protocols/xdg_dirs.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <print>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

  constexpr char kCallerCwdSeparator = '\x1e';
  constexpr auto kTimeout = std::chrono::seconds(2);
  constexpr std::size_t kMaxReplyBytes = 64 * 1024 * 1024;

  std::string resolveSocketPath() {
    const std::string runtime = xdg::runtimeDir();
    if (runtime.empty()) {
      return {};
    }
    const char* display = std::getenv("WAYLAND_DISPLAY");
    if (display == nullptr || display[0] == '\0') {
      display = "wayland-0";
    }
    return runtime + "/noctalia-" + display + ".sock";
  }

} // namespace

int IpcClient::send(const std::string& command, bool stream) {
  const std::string path = resolveSocketPath();
  if (path.empty()) {
    std::println(stderr, "error: XDG_RUNTIME_DIR is not set");
    return 1;
  }

  // Prefix the caller cwd so the daemon resolves relative paths correctly.
  std::string line;
  std::error_code ec;
  const std::filesystem::path cwd = std::filesystem::current_path(ec);
  if (!ec && cwd.is_absolute()) {
    line = cwd.string();
    line += kCallerCwdSeparator;
  }
  line += command;

  const auto deadline = std::chrono::steady_clock::now() + kTimeout;
  std::string response;
  if (!stream) {
    auto reply = ipc::request(path, line, ipc::EndOfRequest::HalfClose, kMaxReplyBytes, deadline);
    switch (reply.status) {
      case ipc::Status::Ok:
        break;
      case ipc::Status::NoServer:
        std::println(stderr, "error: noctalia is not running");
        return 1;
      case ipc::Status::TimedOut:
        std::println(stderr, "error: no reply within {} s", kTimeout.count());
        return 1;
      case ipc::Status::TooLong:
        std::println(stderr, "error: the reply is over {} bytes", kMaxReplyBytes);
        return 1;
      case ipc::Status::Failed:
        std::println(stderr, "error: IPC failed: {}", std::strerror(errno));
        return 1;
    }
    response = std::move(reply.data);
  } else {
    const ipc::Fd fd = ipc::connectTo(path, deadline);
    if (!fd) {
      std::println(stderr, "error: noctalia is not running");
      return 1;
    }
    if (!ipc::writeAll(fd.get(), line, deadline) || ::shutdown(fd.get(), SHUT_WR) < 0) {
      std::println(stderr, "error: sending the request failed: {}", std::strerror(errno));
      return 1;
    }
    // Copies events until the shell exits or drops this subscriber.
    char buf[4096];
    for (;;) {
      const auto n = ::read(fd.get(), buf, sizeof(buf));
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n < 0) {
        std::println(stderr, "error: read() failed: {}", std::strerror(errno));
        return 1;
      }
      if (n == 0) {
        break;
      }
      std::fwrite(buf, 1, static_cast<std::size_t>(n), stdout);
      std::fflush(stdout);
    }
  }

  if (stream) {
    // A stream ends only when the shell exits or drops this subscriber.
    std::fputs("error: the event stream ended\n", stderr);
    return 1;
  }
  std::fputs(response.c_str(), stdout);
  if (response.starts_with('{')) {
    const auto reply = nlohmann::json::parse(response, nullptr, false);
    if (reply.is_object() && reply.contains("ok") && reply["ok"].is_boolean()) {
      return reply["ok"].get<bool>() ? 0 : 1;
    }
  }
  return response.starts_with("error:") ? 1 : 0;
}
