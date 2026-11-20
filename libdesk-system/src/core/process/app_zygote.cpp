#include "core/process/app_zygote.h"

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace process::zygote {

  namespace {
    constexpr std::string_view kMagic = "andromeda-zygote-1";
  } // namespace

  std::filesystem::path socketPath(std::string_view appId) {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime == nullptr || runtime[0] != '/') {
      return {};
    }
    return std::filesystem::path(runtime) / "andromeda" / "zygote" / (std::string(appId) + ".sock");
  }

  std::string encode(const Request& request) {
    std::string out;
    for (const std::string_view field : {kMagic, std::string_view(request.workingDir), std::string_view(request.activationToken)}) {
      out.append(field).push_back('\0');
    }
    for (const auto& arg : request.argv) {
      out.append(arg).push_back('\0');
    }
    return out;
  }

  std::optional<Request> decode(std::span<const char> message) {
    std::vector<std::string> fields;
    std::string_view rest(message.data(), message.size());
    while (!rest.empty()) {
      const auto end = rest.find('\0');
      if (end == std::string_view::npos) {
        return std::nullopt;
      }
      fields.emplace_back(rest.substr(0, end));
      rest.remove_prefix(end + 1);
    }
    if (fields.size() < 4 || fields[0] != kMagic) {
      return std::nullopt;
    }
    Request request;
    request.workingDir = std::move(fields[1]);
    request.activationToken = std::move(fields[2]);
    request.argv.assign(std::make_move_iterator(fields.begin() + 3), std::make_move_iterator(fields.end()));
    return request;
  }

  std::optional<pid_t> launch(std::string_view appId, const Request& request) {
    const auto path = socketPath(appId);
    sockaddr_un addr{};
    if (path.empty() || path.native().size() >= sizeof(addr.sun_path)) {
      return std::nullopt;
    }
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return std::nullopt;
    }
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.native().size());
    const timeval timeout{.tv_sec = 1, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    std::optional<pid_t> pid;
    const std::string message = encode(request);
    if (connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0
        && send(fd, message.data(), message.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(message.size())) {
      std::int32_t reply = -1;
      if (recv(fd, &reply, sizeof(reply), 0) == sizeof(reply) && reply > 0) {
        pid = reply;
      }
    }
    close(fd);
    return pid;
  }

} // namespace process::zygote
