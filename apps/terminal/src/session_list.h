#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace term {

  using SessionClock = std::chrono::steady_clock;

  enum class SessionKind : std::uint8_t { Shell, App, Ssh, Container, Agent };

  struct CommandIdle {
    bool operator==(const CommandIdle&) const = default;
  };
  // Active while the command prints; Quiet after component.terminal.command_quiet_ms without output.
  enum class CommandOutput : std::uint8_t { Active, Quiet };
  struct CommandRunning {
    std::string command;
    SessionClock::time_point start;
    CommandOutput output = CommandOutput::Active;
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
    std::string host; // Ssh or Container: the host or container name, when known
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
    struct Output {
      std::uint64_t id;
      CommandOutput output;
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
      std::string host;
    };
    // Puts `joining` beside `target` (in target's split, or a new one), right after it in pane order or right
    // before it, and focuses it.
    struct Join {
      std::uint64_t target;
      std::uint64_t joining;
      bool before = false;
    };
    struct Leave {
      std::uint64_t id;
    };
  } // namespace session_event

  using SessionListEvent = std::variant<session_event::Open, session_event::Close, session_event::Show,
                                        session_event::CommandStart, session_event::CommandEnd, session_event::Rename,
                                        session_event::Cwd, session_event::Kind, session_event::Join,
                                        session_event::Leave, session_event::Output>;

  enum class ApplyResult : std::uint8_t { Applied, Rejected, LastClosed };

  inline constexpr std::size_t kMaxPanes = 4;

  // The sessions of one window. Invariants: exactly one session is shown while the list is non-empty; every visible
  // session is Seen; only a finished command can leave a session Unseen; recent() orders ids most recent first.
  // A split holds 2..kMaxPanes distinct sessions in layout order; a session is in at most one split. Showing a member
  // shows its whole split with that member focused (the shown session is the focused pane).
  class SessionList {
  public:
    // The only transition function. A rejected event leaves the list unchanged.
    ApplyResult apply(const SessionListEvent& event);

    [[nodiscard]] const std::vector<SessionInfo>& sessions() const noexcept { return m_sessions; }
    [[nodiscard]] const std::vector<std::uint64_t>& recent() const noexcept { return m_recent; }
    [[nodiscard]] std::optional<std::uint64_t> shown() const noexcept;
    [[nodiscard]] const SessionInfo* find(std::uint64_t id) const noexcept;
    [[nodiscard]] const std::vector<std::vector<std::uint64_t>>& splits() const noexcept { return m_splits; }
    [[nodiscard]] const std::vector<std::uint64_t>* splitOf(std::uint64_t id) const noexcept;
    // The panes on screen in layout order: the shown session's split, or the shown session alone.
    [[nodiscard]] std::vector<std::uint64_t> visible() const;

    bool operator==(const SessionList&) const = default;

  private:
    SessionInfo* get(std::uint64_t id) noexcept;
    void show(std::uint64_t id);
    void removeFromSplit(std::uint64_t id);

    std::vector<SessionInfo> m_sessions;
    std::vector<std::uint64_t> m_recent;
    std::vector<std::vector<std::uint64_t>> m_splits;
  };

} // namespace term
