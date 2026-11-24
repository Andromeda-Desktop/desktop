#include "compositors/andromeda/andromeda_runtime.h"

#include "compositors/andromeda/andromeda_event_handler.h"
#include "core/log.h"
#include "protocols/ipc_channel.h"
#include "protocols/xdg_dirs.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace compositors::andromeda {

  namespace {

    constexpr Logger kLog("andromeda_runtime");
    constexpr auto kRequestTimeout = std::chrono::milliseconds(500);
    constexpr std::size_t kReadBufferMaxBytes = 1024U * 1024U;
    constexpr std::string_view kSlideFamily = "workspace_slide";
    // A request of its own: a compositor that predates workspace_slide rejects only this one.
    constexpr std::string_view kSlideStreamRequest = R"({"cmd":"subscribe","events":["workspace_slide"]})";
    constexpr std::string_view kSlideStreamUnknown = "unknown subscription event: workspace_slide";

    [[nodiscard]] bool isSocketPath(const std::string& path) {
      struct stat st{};
      return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
    }

  } // namespace

  struct AndromedaRuntime::IpcReply {
    std::optional<nlohmann::json> json;
  };

  bool AndromedaRuntime::available() const {
    ensureResolved();
    return !m_socketPath.empty();
  }

  const std::string& AndromedaRuntime::socketPath() const {
    ensureResolved();
    return m_socketPath;
  }

  std::optional<nlohmann::json>
  AndromedaRuntime::requestCommand(std::string_view cmd, std::optional<std::string_view> arg) const {
    nlohmann::json request = nlohmann::json::object();
    request["cmd"] = cmd;
    if (arg.has_value()) {
      request["arg"] = *arg;
    }
    auto payload = request.dump();
    payload.push_back('\n');

    const auto reply = this->request(payload);
    if (!reply.json.has_value() || !reply.json->is_object()) {
      return std::nullopt;
    }
    if (reply.json->contains("err") || !reply.json->contains("ok")) {
      return std::nullopt;
    }
    return reply.json;
  }

  bool AndromedaRuntime::requestAction(std::string_view action) const {
    return requestCommand("msg", action).has_value();
  }

  AndromedaRuntime::IpcReply AndromedaRuntime::request(std::string_view payload) const {
    ensureResolved();
    if (m_socketPath.empty() || payload.empty()) {
      return {};
    }
    const auto reply = ipc::request(m_socketPath, payload, ipc::EndOfRequest::KeepOpen, kReadBufferMaxBytes,
                                    std::chrono::steady_clock::now() + kRequestTimeout);
    if (reply.status != ipc::Status::Ok) {
      return {};
    }
    const std::string_view line = std::string_view(reply.data).substr(0, reply.data.find('\n'));
    if (line.empty()) {
      return {};
    }
    try {
      return {std::make_optional(nlohmann::json::parse(line))};
    } catch (const nlohmann::json::exception&) {
      return {};
    }
  }

  void AndromedaRuntime::refresh() {
    m_socketPath.clear();
    m_resolved = false;
    resolveSocketPath();
  }

  AndromedaRuntime::~AndromedaRuntime() { closeSocket(false); }

  void AndromedaRuntime::cleanup() {
    closeSocket(false);
    m_readBuffer.clear();
    m_reconnectBackoff.reset();
    notifyStreamReset();
  }

  std::vector<std::string_view> AndromedaRuntime::newFamilies(const AndromedaEventHandler* added) const {
    std::vector<std::string_view> wanted;
    const auto collect = [&wanted](const AndromedaEventHandler& handler) {
      for (const auto family : handler.families()) {
        if (!std::ranges::contains(wanted, family)) {
          wanted.push_back(family);
        }
      }
    };
    if (added == nullptr) {
      for (const auto* handler : m_eventHandlers) {
        collect(*handler);
      }
      return wanted;
    }
    std::vector<std::string_view> known;
    for (const auto* handler : m_eventHandlers) {
      known.insert(known.end(), handler->families().begin(), handler->families().end());
    }
    collect(*added);
    std::erase_if(wanted, [&known](std::string_view family) { return std::ranges::contains(known, family); });
    return wanted;
  }

  std::string AndromedaRuntime::subscribeRequests(const std::vector<std::string_view>& families) {
    auto plain = families;
    const auto wantsSlide = std::erase(plain, kSlideFamily) > 0;
    std::string requests;
    if (!plain.empty()) {
      requests = nlohmann::json{{"cmd", "subscribe"}, {"events", plain}}.dump();
      requests.push_back('\n');
    }
    if (wantsSlide) {
      requests.append(kSlideStreamRequest);
      requests.push_back('\n');
    }
    return requests;
  }

  void AndromedaRuntime::registerEventHandler(AndromedaEventHandler* handler) {
    if (handler == nullptr || std::ranges::contains(m_eventHandlers, handler)) {
      return;
    }
    const auto fresh = newFamilies(handler);
    m_eventHandlers.push_back(handler);
    if (m_eventSocketFd >= 0) {
      if (!ipc::writeAll(m_eventSocketFd, subscribeRequests(fresh), std::chrono::steady_clock::now() + kRequestTimeout)) {
        closeSocket(true);
      }
    } else if (available()) {
      connectIfNeeded();
    }
  }

  void AndromedaRuntime::unregisterEventHandler(AndromedaEventHandler* handler) {
    std::erase(m_eventHandlers, handler);
  }

  void AndromedaRuntime::dispatchEvent(std::string_view event, const nlohmann::json& data) const {
    for (auto* handler : m_eventHandlers) {
      if (handler != nullptr) {
        handler->handleEvent(event, data);
      }
    }
  }

  void AndromedaRuntime::notifyStreamReset() const {
    for (auto* handler : m_eventHandlers) {
      if (handler != nullptr) {
        handler->handleStreamReset();
      }
    }
  }

  int AndromedaRuntime::pollTimeoutMs() const noexcept {
    if (m_eventSocketFd >= 0 || !available()) {
      return -1;
    }
    if (m_nextReconnectAt.time_since_epoch().count() == 0) {
      return 0;
    }
    const auto remaining =
        std::chrono::ceil<std::chrono::milliseconds>(m_nextReconnectAt - std::chrono::steady_clock::now()).count();
    return static_cast<int>(std::max<std::int64_t>(0, remaining));
  }

  void AndromedaRuntime::dispatchPoll(short revents) {
    if (!available()) {
      return;
    }
    if (m_eventSocketFd < 0) {
      connectIfNeeded();
      return;
    }
    if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      closeSocket(true);
      return;
    }
    if ((revents & POLLIN) != 0) {
      readSocket();
    }
  }

  void AndromedaRuntime::connectIfNeeded() {
    ensureResolved();
    if (m_eventSocketFd >= 0 || m_socketPath.empty()) {
      return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_nextReconnectAt.time_since_epoch().count() != 0 && now < m_nextReconnectAt) {
      return;
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      scheduleReconnect();
      return;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (m_socketPath.size() >= sizeof(addr.sun_path)) {
      kLog.warn("andromeda socket path too long");
      ::close(fd);
      scheduleReconnect();
      return;
    }
    std::memcpy(addr.sun_path, m_socketPath.c_str(), m_socketPath.size() + 1);

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
      ::close(fd);
      scheduleReconnect();
      return;
    }

    const std::string subscribe = subscribeRequests(newFamilies(nullptr));
    if (!ipc::writeAll(fd, subscribe, std::chrono::steady_clock::now() + kRequestTimeout)) {
      ::close(fd);
      scheduleReconnect();
      return;
    }

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
      (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    m_eventSocketFd = fd;
    m_nextReconnectAt = {};
    m_reconnectBackoff.reset();
    m_readBuffer.clear();
    kLog.debug("connected to andromeda event stream");
  }

  void AndromedaRuntime::closeSocket(bool scheduleReconnectFlag) {
    if (m_eventSocketFd >= 0) {
      ::close(m_eventSocketFd);
      m_eventSocketFd = -1;
      notifyStreamReset();
    }

    if (scheduleReconnectFlag) {
      scheduleReconnect();
    } else {
      m_nextReconnectAt = {};
    }
  }

  void AndromedaRuntime::scheduleReconnect() {
    const auto now = std::chrono::steady_clock::now();
    m_nextReconnectAt = now + m_reconnectBackoff.nextDelay();
  }

  void AndromedaRuntime::readSocket() {
    std::array<char, 4096> buffer{};
    while (true) {
      const ssize_t readBytes = ::read(m_eventSocketFd, buffer.data(), buffer.size());
      if (readBytes > 0) {
        m_readBuffer.insert(m_readBuffer.end(), buffer.begin(), buffer.begin() + readBytes);
        if (m_readBuffer.size() > kReadBufferMaxBytes) {
          kLog.warn("andromeda event stream read buffer exceeded {} bytes; reconnecting", kReadBufferMaxBytes);
          closeSocket(true);
          m_readBuffer.clear();
          return;
        }
        continue;
      }

      if (readBytes == 0) {
        closeSocket(true);
        return;
      }

      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }

      closeSocket(true);
      return;
    }

    parseMessages();
  }

  void AndromedaRuntime::parseMessages() {
    auto lineStart = m_readBuffer.begin();
    for (auto it = m_readBuffer.begin(); it != m_readBuffer.end(); ++it) {
      if (*it != '\n') {
        continue;
      }

      std::string line(lineStart, it);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }

      if (!line.empty() && !handleMessage(line)) {
        m_readBuffer.clear();
        return;
      }

      lineStart = std::next(it);
    }

    if (lineStart != m_readBuffer.begin()) {
      m_readBuffer.erase(m_readBuffer.begin(), lineStart);
    }
  }

  bool AndromedaRuntime::handleMessage(std::string_view line) {
    nlohmann::json json;
    try {
      json = nlohmann::json::parse(line);
    } catch (const nlohmann::json::exception& e) {
      kLog.warn("failed to parse andromeda event stream message: {}", e.what());
      return true;
    }

    if (!json.is_object()) {
      return true;
    }

    if (const auto err = json.find("err"); err != json.end()) {
      if (err->is_string() && err->get_ref<const std::string&>() == kSlideStreamUnknown) {
        if (!std::exchange(m_slideStreamUnsupportedLogged, true)) {
          kLog.info("compositor has no workspace_slide event; the bar's workspace capsule keeps its own springs");
        }
        return true;
      }
      kLog.warn("andromeda event stream returned an error, reconnecting");
      closeSocket(true);
      return false;
    }

    const auto eventIt = json.find("event");
    if (eventIt == json.end() || !eventIt->is_string()) {
      return true;
    }

    dispatchEvent(eventIt->get_ref<const std::string&>(), json.value("data", nlohmann::json()));
    return true;
  }

  void AndromedaRuntime::ensureResolved() const {
    if (!m_resolved) {
      resolveSocketPath();
    }
  }

  void AndromedaRuntime::resolveSocketPath() const {
    m_resolved = true;

    if (const char* socketPath = std::getenv("ANDROMEDA_SOCKET"); socketPath != nullptr && socketPath[0] != '\0') {
      m_socketPath = socketPath;
      return;
    }

    const std::string runtimeDir = xdg::runtimeDir();
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    if (runtimeDir.empty() || waylandDisplay == nullptr || waylandDisplay[0] == '\0') {
      return;
    }

    const std::string fallback = runtimeDir + "/andromeda-" + waylandDisplay + ".sock";
    if (isSocketPath(fallback)) {
      m_socketPath = fallback;
    }
  }

} // namespace compositors::andromeda
