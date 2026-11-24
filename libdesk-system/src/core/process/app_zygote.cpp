#include "core/process/app_zygote.h"

#include "protocols/xdg_dirs.h"
#include "util/string_utils.h"

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace process::zygote {

  namespace {
    constexpr std::string_view kMagic = "andromeda-zygote-3";
    constexpr std::string_view kLaunch = "launch";
    constexpr std::string_view kStatus = "status";
    // Less than the client's 1 s wait, so a child forked before the deadline is reported before the client gives up.
    constexpr auto kServeWindow = std::chrono::milliseconds(500);

    // A connected SEQPACKET socket with 1 s send and receive timeouts, or -1.
    int connectTo(const std::filesystem::path& path) {
      sockaddr_un addr{};
      if (path.empty() || path.native().size() >= sizeof(addr.sun_path)) {
        return -1;
      }
      const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
      if (fd < 0) {
        return -1;
      }
      addr.sun_family = AF_UNIX;
      std::memcpy(addr.sun_path, path.c_str(), path.native().size());
      const timeval timeout{.tv_sec = 1, .tv_usec = 0};
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      if (connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
      }
      return fd;
    }

    bool sendMessage(int fd, const Message& message) {
      const std::string bytes = encode(message);
      return send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(bytes.size());
    }

    std::optional<std::int32_t> receiveReply(int fd) {
      std::int32_t reply = 0;
      if (recv(fd, &reply, sizeof(reply), 0) != sizeof(reply)) {
        return std::nullopt;
      }
      return reply;
    }
  } // namespace

  std::filesystem::path socketPath(std::string_view appId) {
    const std::string runtime = xdg::runtimeDir();
    if (runtime.empty()) {
      return {};
    }
    return std::filesystem::path(runtime) / "andromeda" / "zygote" / (std::string(appId) + ".sock");
  }

  std::string encode(const Message& message) {
    std::string out;
    const auto field = [&out](std::string_view value) { out.append(value).push_back('\0'); };
    field(kMagic);
    if (const auto* status = std::get_if<StatusQuery>(&message)) {
      field(kStatus);
      field(status->id);
      return out;
    }
    const auto& request = std::get<Request>(message);
    field(kLaunch);
    field(request.id);
    field(std::to_string(request.deadline.time_since_epoch().count()));
    field(request.workingDir);
    field(request.activationToken);
    for (const auto& arg : request.argv) {
      field(arg);
    }
    return out;
  }

  std::optional<Message> decode(std::span<const char> message) {
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
    if (fields.size() < 3 || fields[0] != kMagic || fields[2].empty()) {
      return std::nullopt;
    }
    if (fields[1] == kStatus && fields.size() == 3) {
      return StatusQuery{.id = std::move(fields[2])};
    }
    std::chrono::steady_clock::rep deadline = 0;
    if (fields[1] != kLaunch || fields.size() < 7
        || std::from_chars(fields[3].data(), fields[3].data() + fields[3].size(), deadline).ec != std::errc{}) {
      return std::nullopt;
    }
    Request request;
    request.id = std::move(fields[2]);
    request.deadline = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(deadline));
    request.workingDir = std::move(fields[4]);
    request.activationToken = std::move(fields[5]);
    request.argv.assign(std::make_move_iterator(fields.begin() + 6), std::make_move_iterator(fields.end()));
    return request;
  }

  LaunchResult launch(std::string_view appId, const Request& request) {
    using enum LaunchResult::Outcome;
    const auto toResult = [](std::int32_t reply) {
      return reply > 0 ? LaunchResult{.outcome = Started, .pid = reply} : LaunchResult{.outcome = Refused};
    };
    const auto path = socketPath(appId);
    Request timed = request;
    timed.id = StringUtils::generateUuid();
    timed.deadline = std::chrono::steady_clock::now() + kServeWindow;
    const int fd = timed.id.empty() ? -1 : connectTo(path);
    if (fd < 0) {
      return {.outcome = Refused};
    }
    // SEQPACKET sends a whole record or nothing: a failed send never reaches the zygote.
    if (!sendMessage(fd, timed)) {
      close(fd);
      return {.outcome = Refused};
    }
    const auto reply = receiveReply(fd);
    close(fd);
    if (reply) {
      return toResult(*reply);
    }
    const int statusFd = connectTo(path);
    if (statusFd < 0) {
      return {.outcome = Unknown};
    }
    const auto status = sendMessage(statusFd, StatusQuery{.id = timed.id}) ? receiveReply(statusFd) : std::nullopt;
    close(statusFd);
    return status && *status != 0 ? toResult(*status) : LaunchResult{.outcome = Unknown};
  }

} // namespace process::zygote
