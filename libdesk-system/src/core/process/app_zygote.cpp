#include "core/process/app_zygote.h"
#include "protocols/ipc_channel.h"

#include "protocols/xdg_dirs.h"
#include "util/string_utils.h"

#include <poll.h>
#include <sys/socket.h>
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
      return ipc::connectTo(path, std::chrono::steady_clock::now() + std::chrono::seconds(1), SOCK_SEQPACKET).release();
    }

    bool sendMessage(int fd, const Message& message) {
      const std::string bytes = encode(message);
      return send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(bytes.size());
    }

    struct Reply {
      std::int32_t value = 0;
      ipc::Fd pidfd; // empty from a zygote that sends none
    };

    // Waits up to 1 s.
    std::optional<Reply> receiveReply(int fd) {
      pollfd ready{.fd = fd, .events = POLLIN, .revents = 0};
      if (poll(&ready, 1, 1000) != 1) {
        return std::nullopt;
      }
      std::int32_t value = 0;
      auto received = ipc::recvFds(fd, std::as_writable_bytes(std::span(&value, 1)), 1);
      if (received.kind != ipc::Received::Kind::Data || received.size != sizeof(value)) {
        return std::nullopt;
      }
      return Reply{.value = value, .pidfd = received.fds.empty() ? ipc::Fd{} : std::move(received.fds.front())};
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
    const auto toResult = [](Reply& reply) {
      return reply.value > 0 ? LaunchResult{.outcome = Started, .pid = reply.value, .pidfd = std::move(reply.pidfd)}
                             : LaunchResult{.outcome = Refused};
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
    auto reply = receiveReply(fd);
    close(fd);
    if (reply) {
      return toResult(*reply);
    }
    const int statusFd = connectTo(path);
    if (statusFd < 0) {
      return {.outcome = Unknown};
    }
    auto status = sendMessage(statusFd, StatusQuery{.id = timed.id}) ? receiveReply(statusFd) : std::nullopt;
    close(statusFd);
    return status && status->value != 0 ? toResult(*status) : LaunchResult{.outcome = Unknown};
  }

} // namespace process::zygote
