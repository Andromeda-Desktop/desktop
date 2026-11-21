#include "agent/agent_sessions.h"

#include "core/log.h"
#include "input/seat.h"
#include "server/server.h"
#include "wlr.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server-core.h>

namespace andromeda {

  namespace {
    constexpr Logger kLog("agent");
  } // namespace

  AgentSessions::AgentSessions(Server& server) : m_server(&server) {}

  AgentSessions::~AgentSessions() {
    for (const auto& session : m_sessions) {
      closeSocket(*session);
    }
  }

  std::expected<AgentSessions::Started, std::string> AgentSessions::start() {
    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    if (runtimeDir == nullptr || runtimeDir[0] == '\0') {
      return std::unexpected("XDG_RUNTIME_DIR is not set");
    }
    auto session = std::make_unique<Session>();
    session->owner = this;
    session->id = m_nextId;
    session->socketPath = std::format("{}/{}-agent-{}", runtimeDir, m_server->socketName(), session->id);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (session->socketPath.size() >= sizeof(addr.sun_path)) {
      return std::unexpected("agent socket path too long");
    }
    session->socketPath.copy(addr.sun_path, session->socketPath.size());
    session->listenFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (session->listenFd < 0) {
      return std::unexpected(std::format("agent socket: {}", strerror(errno)));
    }
    unlink(session->socketPath.c_str());
    if (bind(session->listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0
        || chmod(session->socketPath.c_str(), S_IRUSR | S_IWUSR) < 0
        || listen(session->listenFd, 16) < 0) {
      const std::string error = std::format("agent socket {}: {}", session->socketPath, strerror(errno));
      closeSocket(*session);
      return std::unexpected(error);
    }
    session->listenSource = wl_event_loop_add_fd(
        wl_display_get_event_loop(m_server->display()), session->listenFd, WL_EVENT_READABLE, onListenReadable,
        session.get()
    );
    if (session->listenSource == nullptr) {
      closeSocket(*session);
      return std::unexpected("failed to watch the agent socket");
    }
    session->seat = wlr_seat_create(m_server->display(), std::format("agent-{}", session->id).c_str());
    if (session->seat == nullptr) {
      closeSocket(*session);
      return std::unexpected("failed to create the agent seat");
    }
    ++m_nextId;
    Started started{.id = session->id, .socketPath = session->socketPath};
    kLog.info("agent session {} listening on {}", session->id, session->socketPath);
    m_sessions.push_back(std::move(session));
    return started;
  }

  std::expected<void, std::string> AgentSessions::stop(uint32_t id) {
    Session* session = find(id);
    if (session == nullptr) {
      return std::unexpected(std::format("no agent session {}", id));
    }
    if (session->state == State::Ended) {
      return std::unexpected(std::format("agent session {} already ended", id));
    }
    closeSocket(*session);
    session->state = State::Ended;
    kLog.info("agent session {} ended", id);
    return {};
  }

  nlohmann::json AgentSessions::list() const {
    nlohmann::json sessions = nlohmann::json::array();
    for (const auto& session : m_sessions) {
      sessions.push_back({
          {"id", session->id},
          {"state", session->state == State::Running ? "running" : "ended"},
          {"socket", session->socketPath},
          {"seat", std::format("agent-{}", session->id)},
      });
    }
    return sessions;
  }

  bool AgentSessions::seatVisible(const wl_global* global, Domain domain) const {
    if (domain.kind == Domain::Kind::User) {
      return m_server->seat() != nullptr && global == m_server->seat()->wlr()->global;
    }
    const Session* session = find(domain.session);
    return session != nullptr && global == session->seat->global;
  }

  int AgentSessions::onListenReadable(int /*fd*/, uint32_t /*mask*/, void* data) {
    auto* session = static_cast<Session*>(data);
    session->owner->accept(*session);
    return 0;
  }

  void AgentSessions::accept(Session& session) {
    while (true) {
      const int fd = accept4(session.listenFd, nullptr, nullptr, SOCK_CLOEXEC);
      if (fd < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          kLog.warn("agent session {}: accept failed: {}", session.id, strerror(errno));
        }
        return;
      }
      ucred peer{};
      socklen_t size = sizeof(peer);
      if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || peer.uid != geteuid()) {
        kLog.warn("agent session {}: refused a connection from uid {}", session.id, peer.uid);
        close(fd);
        continue;
      }
      wl_client* client = wl_client_create(m_server->display(), fd);
      if (client == nullptr) {
        kLog.warn("agent session {}: wl_client_create failed", session.id);
        close(fd);
        continue;
      }
      m_server->setClientDomain(client, Domain{.kind = Domain::Kind::Agent, .session = session.id});
    }
  }

  void AgentSessions::closeSocket(Session& session) {
    if (session.listenSource != nullptr) {
      wl_event_source_remove(session.listenSource);
      session.listenSource = nullptr;
    }
    if (session.listenFd >= 0) {
      close(session.listenFd);
      session.listenFd = -1;
      unlink(session.socketPath.c_str());
    }
  }

  AgentSessions::Session* AgentSessions::find(uint32_t id) const {
    for (const auto& session : m_sessions) {
      if (session->id == id) {
        return session.get();
      }
    }
    return nullptr;
  }

} // namespace andromeda
