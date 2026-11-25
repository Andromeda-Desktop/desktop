#include "andromeda/ipc.h"

#include "core/fd_watch.h"
#include "core/poll_loop.h"
#include "protocols/ipc_channel.h"
#include "protocols/xdg_dirs.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <string_view>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace xdpu {

  namespace {

    constexpr std::size_t kMaxEventBytes = 1024U * 1024U; // one event line: a window list with many windows

    std::string socketPath() {
      if (const char* configured = std::getenv("ANDROMEDA_SOCKET"); configured != nullptr && configured[0] != '\0') {
        return configured;
      }
      const std::string runtime = xdg::runtimeDir();
      const char* display = std::getenv("WAYLAND_DISPLAY");
      if (runtime.empty() || display == nullptr || display[0] == '\0') {
        return {};
      }
      return runtime + "/andromeda-" + display + ".sock";
    }

    void closeFd(int fd) {
      if (fd >= 0) {
        while (::close(fd) < 0 && errno == EINTR) {
        }
      }
    }

  } // namespace

  struct AndromedaIpc::Impl {
    PollLoop& loop;
    ScreenCastHandler screenCastHandler;
    FocusHandler focusedWindowHandler;
    FocusHandler focusedOutputHandler;
    CaptureBlockedHandler captureBlockedHandler;
    int fd = -1;
    std::unique_ptr<FdWatch> watch;
    ipc::LineReader input{kMaxEventBytes};
    std::string output;
    size_t writeOffset = 0;

    Impl(
        PollLoop& loop, ScreenCastHandler screenCastHandler, FocusHandler focusedWindowHandler,
        FocusHandler focusedOutputHandler, CaptureBlockedHandler captureBlockedHandler
    )
        : loop(loop), screenCastHandler(std::move(screenCastHandler)),
          focusedWindowHandler(std::move(focusedWindowHandler)), focusedOutputHandler(std::move(focusedOutputHandler)),
          captureBlockedHandler(std::move(captureBlockedHandler)) {
      const std::string path = socketPath();
      if (path.empty()) {
        std::fprintf(stderr, "andromeda-ipc: compositor socket path is unavailable\n");
        return;
      }
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      ipc::Fd conn = ipc::connectTo(path, deadline);
      if (!conn) {
        std::fprintf(stderr, "andromeda-ipc: cannot connect to %s\n", path.c_str());
        return;
      }
      constexpr std::string_view request =
          "{\"cmd\":\"subscribe\",\"events\":[\"screencast\",\"windows\",\"workspaces\"]}\n";
      if (!ipc::writeAll(conn.get(), request, deadline)) {
        std::fprintf(stderr, "andromeda-ipc: subscribe failed: %s\n", std::strerror(errno));
        return;
      }
      fd = conn.release();

      const int flags = fcntl(fd, F_GETFL, 0);
      if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        std::fprintf(stderr, "andromeda-ipc: failed to make socket nonblocking: %s\n", std::strerror(errno));
        closeFd(fd);
        fd = -1;
        return;
      }

      watch = std::make_unique<FdWatch>(loop, fd, POLLIN, [this](short events) { readable(events); });
    }

    ~Impl() {
      watch.reset();
      closeFd(fd);
    }

    void disconnect() {
      watch.reset();
      closeFd(fd);
      fd = -1;
      output.clear();
      writeOffset = 0;
    }

    void flushOutput() {
      while (fd >= 0 && writeOffset < output.size()) {
        const ssize_t size = send(fd, output.data() + writeOffset, output.size() - writeOffset, MSG_NOSIGNAL);
        if (size > 0) {
          writeOffset += static_cast<size_t>(size);
          continue;
        }
        if (size < 0 && errno == EINTR) {
          continue;
        }
        if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          watch->setEvents(POLLIN | POLLOUT);
          return;
        }
        std::fprintf(stderr, "andromeda-ipc: send failed: %s\n", std::strerror(errno));
        disconnect();
        return;
      }
      output.clear();
      writeOffset = 0;
      if (watch) {
        watch->setEvents(POLLIN);
      }
    }

    void setScreenCastActive(bool active) {
      if (fd < 0) {
        return;
      }
      output += nlohmann::json{{"cmd", "screencast-session"}, {"active", active}}.dump();
      output.push_back('\n');
      flushOutput();
    }

    void readable(short events) {
      if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        disconnect();
        return;
      }
      if ((events & POLLOUT) != 0) {
        flushOutput();
        if (fd < 0) {
          return;
        }
      }
      if ((events & POLLIN) == 0) {
        return;
      }

      do {
        while (const auto line = input.nextLine()) {
          handleLine(*line);
        }
      } while (input.readFrom(fd) > 0);
      while (const auto line = input.nextLine()) {
        handleLine(*line);
      }
      if (input.state() == ipc::LineReader::State::TooLong) {
        std::fprintf(stderr, "andromeda-ipc: an event is longer than %zu bytes\n", kMaxEventBytes);
      }
      if (input.state() != ipc::LineReader::State::Reading) {
        disconnect();
      }
    }

    void handleLine(std::string_view line) {
      const auto event = nlohmann::json::parse(line, nullptr, false);
      if (event.is_discarded() || !event.is_object()) {
        return;
      }
      const auto data = event.find("data");
      if (data == event.end()) {
        return;
      }

      const std::string eventName = event.value("event", "");
      if (eventName == "windows") {
        std::optional<std::string> focused;
        std::set<std::string> blocked;
        if (data->is_array()) {
          for (const auto& window : *data) {
            if (!window.is_object()) {
              continue;
            }
            const std::string identifier = window.value("id", "");
            if (identifier.empty()) {
              continue;
            }
            if (!focused && window.value("active", false)) {
              focused = identifier;
            }
            if (window.value("capture_blocked", false)) {
              blocked.insert(identifier);
            }
          }
        }
        if (focusedWindowHandler) {
          focusedWindowHandler(focused);
        }
        if (captureBlockedHandler) {
          captureBlockedHandler(std::move(blocked));
        }
        return;
      }
      if (eventName == "workspaces") {
        std::optional<std::string> focused;
        if (data->is_array()) {
          for (const auto& workspace : *data) {
            if (workspace.is_object() && workspace.value("focused", false)) {
              const std::string output = workspace.value("output", "");
              if (!output.empty()) {
                focused = output;
              }
              break;
            }
          }
        }
        if (focusedOutputHandler) {
          focusedOutputHandler(focused);
        }
        return;
      }
      if (eventName != "screencast" || !data->is_object()) {
        return;
      }

      ScreenCastCommand command;
      if (const auto serial = data->find("serial"); serial != data->end() && serial->is_number_unsigned()) {
        command.serial = serial->get<uint64_t>();
      } else {
        return;
      }

      const std::string kind = data->value("kind", "");
      if (kind == "clear") {
        command.kind = ScreenCastCommand::Kind::Clear;
      } else if (kind == "output") {
        command.kind = ScreenCastCommand::Kind::Output;
        command.value = data->value("output", "");
      } else if (kind == "window") {
        command.kind = ScreenCastCommand::Kind::Window;
        command.value = data->value("identifier", "");
      } else if (kind == "follow_window") {
        command.kind = ScreenCastCommand::Kind::FollowWindow;
      } else if (kind == "follow_output") {
        command.kind = ScreenCastCommand::Kind::FollowOutput;
      } else if (kind == "follow_stop") {
        command.kind = ScreenCastCommand::Kind::FollowStop;
      } else {
        return;
      }
      if ((command.kind == ScreenCastCommand::Kind::Output || command.kind == ScreenCastCommand::Kind::Window)
          && command.value.empty()) {
        return;
      }
      if (screenCastHandler) {
        screenCastHandler(command);
      }
    }
  };

  AndromedaIpc::AndromedaIpc(
      PollLoop& loop, ScreenCastHandler screenCastHandler, FocusHandler focusedWindowHandler,
      FocusHandler focusedOutputHandler, CaptureBlockedHandler captureBlockedHandler
  )
      : m_impl(
            std::make_unique<Impl>(
                loop, std::move(screenCastHandler), std::move(focusedWindowHandler), std::move(focusedOutputHandler),
                std::move(captureBlockedHandler)
            )
        ) {}

  AndromedaIpc::~AndromedaIpc() = default;

  bool AndromedaIpc::connected() const { return m_impl->fd >= 0; }

  void AndromedaIpc::setScreenCastActive(bool active) { m_impl->setScreenCastActive(active); }

} // namespace xdpu
