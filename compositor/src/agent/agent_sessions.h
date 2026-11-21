#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

struct wl_client;
struct wl_event_source;
struct wl_global;
struct wlr_seat;

namespace andromeda {

  class Server;

  // The input domain of a Wayland connection. It is fixed when the connection is accepted and never changes.
  struct Domain {
    enum class Kind : uint8_t { User, Agent };
    Kind kind = Kind::User;
    uint32_t session = 0; // Agent only
  };

  // Agent sessions: each one owns a Wayland socket and a wlr_seat. A client accepted on that socket is in the
  // session's domain and sees only the session's seat, so agent input never moves the user's focus.
  class AgentSessions {
  public:
    struct Started {
      uint32_t id = 0;
      std::string socketPath;
    };

    explicit AgentSessions(Server& server);
    ~AgentSessions();

    AgentSessions(const AgentSessions&) = delete;
    AgentSessions& operator=(const AgentSessions&) = delete;

    std::expected<Started, std::string> start();
    std::expected<void, std::string> stop(uint32_t id);
    [[nodiscard]] nlohmann::json list() const;

    // True when a client in `domain` may see the wl_seat `global`.
    [[nodiscard]] bool seatVisible(const wl_global* global, Domain domain) const;

  private:
    enum class State : uint8_t { Running, Ended };

    struct Session {
      AgentSessions* owner = nullptr;
      uint32_t id = 0;
      State state = State::Running;
      std::string socketPath;
      int listenFd = -1;
      wl_event_source* listenSource = nullptr;
      wlr_seat* seat = nullptr;
    };

    static int onListenReadable(int fd, uint32_t mask, void* data);
    void accept(Session& session);
    static void closeSocket(Session& session);
    [[nodiscard]] Session* find(uint32_t id) const;

    Server* m_server = nullptr;
    std::vector<std::unique_ptr<Session>> m_sessions;
    uint32_t m_nextId = 1;
  };

} // namespace andromeda
