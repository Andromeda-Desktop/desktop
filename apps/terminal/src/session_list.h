#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace term {

  using SessionClock = std::chrono::steady_clock;

  enum class SessionKind : std::uint8_t { Shell, App, Ssh, Container };

  struct CommandIdle {
    bool operator==(const CommandIdle&) const = default;
  };
  struct CommandRunning {
    std::string command;
    SessionClock::time_point start;
    bool operator==(const CommandRunning&) const = default;
  };
  // exitCode is empty when the shell reports none; such a command counts as succeeded.
  struct CommandFinished {
    std::string command;
    std::optional<int> exitCode;
    SessionClock::duration duration{};
    [[nodiscard]] bool failed() const noexcept { return exitCode.value_or(0) != 0; }
    bool operator==(const CommandFinished&) const = default;
  };
  using CommandState = std::variant<CommandIdle, CommandRunning, CommandFinished>;

  enum class Seen : std::uint8_t { Seen, Unseen };

  struct SessionInfo {
    std::uint64_t id = 0;
    SessionKind kind = SessionKind::Shell;
    std::string name;
    std::string cwd;
    CommandState command;
    Seen seen = Seen::Seen;
    bool operator==(const SessionInfo&) const = default;
  };

  namespace session_event {
    struct Open {
      std::uint64_t id;
      SessionKind kind;
      std::string name;
      std::string cwd;
    };
    struct Close {
      std::uint64_t id;
    };
    struct Show {
      std::uint64_t id;
    };
    struct CommandStart {
      std::uint64_t id;
      std::string command;
      SessionClock::time_point at;
    };
    struct CommandEnd {
      std::uint64_t id;
      std::optional<int> exitCode;
      SessionClock::time_point at;
    };
    struct Rename {
      std::uint64_t id;
      std::string name;
    };
    struct Cwd {
      std::uint64_t id;
      std::string cwd;
    };
    struct Kind {
      std::uint64_t id;
      SessionKind kind;
    };
  } // namespace session_event

  using SessionListEvent = std::variant<session_event::Open, session_event::Close, session_event::Show,
                                        session_event::CommandStart, session_event::CommandEnd, session_event::Rename,
                                        session_event::Cwd, session_event::Kind>;

  enum class ApplyResult : std::uint8_t { Applied, Rejected, LastClosed };

  // The sessions of one window. Invariants: exactly one session is shown while the list is non-empty; the shown
  // session is Seen; only a finished command can leave a session Unseen; recent() orders ids most recent first.
  class SessionList {
  public:
    // The only transition function. A rejected event leaves the list unchanged.
    ApplyResult apply(const SessionListEvent& event);

    [[nodiscard]] const std::vector<SessionInfo>& sessions() const noexcept { return m_sessions; }
    [[nodiscard]] const std::vector<std::uint64_t>& recent() const noexcept { return m_recent; }
    [[nodiscard]] std::optional<std::uint64_t> shown() const noexcept;
    [[nodiscard]] const SessionInfo* find(std::uint64_t id) const noexcept;

    bool operator==(const SessionList&) const = default;

  private:
    SessionInfo* get(std::uint64_t id) noexcept;
    void show(std::uint64_t id);

    std::vector<SessionInfo> m_sessions;
    std::vector<std::uint64_t> m_recent;
  };

} // namespace term
