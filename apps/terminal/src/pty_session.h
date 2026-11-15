#pragma once

#include <ghostty/vt.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <sys/types.h>
#include <thread>
#include <variant>
#include <vector>

namespace term {

enum class SessionState : std::uint8_t { Idle, Running, Exited };

enum class SessionEvent : std::uint8_t { Spawned, ChildExited };

// Returns the next state, or nullopt when the event is invalid in `from`.
std::optional<SessionState> transition(SessionState from, SessionEvent event);

struct CommandMark {
  enum class Kind : std::uint8_t { PromptStart, InputStart, OutputStart, CommandEnd };
  Kind kind;
  std::optional<int> exitCode;
  std::string command; // OutputStart: the command line, when the shell sends it
};

struct SpawnOptions {
  std::vector<std::string> argv;
  std::string cwd;
  std::vector<std::string> env;
  std::uint16_t cols = 80;
  std::uint16_t rows = 24;
  std::uint16_t cellWidthPx = 8;
  std::uint16_t cellHeightPx = 16;
};

// One shell or program on a PTY. A reader thread drains the PTY into a bounded queue while a parser
// thread feeds the terminal, so the program keeps writing during a parse; the UI thread reads terminal state through withTerminal() once per frame.
// The pty lives in andromeda-ptyd when it is reachable, so the program outlives a crash of this process; the reader
// then reads the holder socket, and keys and window size still go straight to a dup of the pty master.
class PtySession {
public:
  // Called on the IO thread when new state is ready, at most once until takeWake().
  using WakeFn = std::function<void()>;
  using MarkFn = std::function<void(const CommandMark&)>;

  PtySession(WakeFn wake, MarkFn onMark = {});
  ~PtySession();
  PtySession(const PtySession&) = delete;
  PtySession& operator=(const PtySession&) = delete;

  bool spawn(const SpawnOptions& options);
  // Takes over an orphaned holder session (holder::listOrphans) and restores its screen.
  bool attach(std::uint64_t id, std::uint16_t cols, std::uint16_t rows, std::uint16_t cellWidthPx,
              std::uint16_t cellHeightPx);
  // The holder session id, or nullopt when the pty is local.
  std::optional<std::uint64_t> heldId() const;
  void write(std::span<const std::uint8_t> bytes);
  void resize(std::uint16_t cols, std::uint16_t rows, std::uint16_t cellWidthPx, std::uint16_t cellHeightPx);

  // Clears the wake latch; call at the start of the frame that consumes the state.
  bool takeWake() { return m_wakePending.exchange(false); }
  // True while the program holds rendering (synchronized output, mode 2026).
  bool renderHeld() const { return m_renderHeld.load(); }

  template <typename F> auto withTerminal(F&& f) {
    std::lock_guard lock(m_termMutex);
    return f(m_terminal);
  }

  SessionState state() const { return m_state.load(); }
  std::optional<int> exitStatus() const;
  std::uint64_t bytesRead() const { return m_bytesRead.load(); }
  // Output offset of the last snapshot sent to the holder: a reattach replays only the output after it.
  std::uint64_t savedOffset() const { return m_savedOffset.load(); }

private:
  struct LocalChild {
    pid_t pid;
  };
  struct HeldSession {
    int socket;
    std::uint64_t id;
  };

  bool newTerminal(std::uint16_t cols, std::uint16_t rows, std::uint16_t cellWidthPx, std::uint16_t cellHeightPx);
  void configureTerminal();
  bool spawnHeld(const SpawnOptions& options);
  bool spawnLocal(const SpawnOptions& options);
  void start();
  void ioLoop();
  void ioLoopHeld(const HeldSession& held);
  void parseLoop();
  void sendSnapshot(const HeldSession& held);
  void endInput(std::optional<int> heldStatus);
  void signalWake();
  void flushOutbound();
  void queueOutbound(std::span<const std::uint8_t> bytes);
  void apply(SessionEvent event);

  static void onWritePty(GhosttyTerminal, void* self, const std::uint8_t* data, std::size_t len);
  static void onRenderHold(GhosttyTerminal, void* self, bool held);
  static void onSemanticPrompt(GhosttyTerminal, void* self, const GhosttyTerminalSemanticPrompt* event);

  WakeFn m_wake;
  MarkFn m_onMark;
  GhosttyTerminal m_terminal = nullptr;
  std::mutex m_termMutex;

  // Set before the threads start and fixed after.
  std::variant<std::monostate, LocalChild, HeldSession> m_link;
  // Local: the pty master. Held: a dup of the holder's master, for writes and TIOCSWINSZ only.
  int m_master = -1;
  int m_eventFd = -1;
  // Holder socket bytes not yet decoded; the reader thread's after start().
  std::vector<std::uint8_t> m_sockBuf;
  // Output offsets: received by the reader thread, parsed by the parser thread.
  std::uint64_t m_received = 0;
  std::uint64_t m_parsed = 0;
  // Output before this offset is a replay after attach: the program got the terminal's replies to it already.
  std::uint64_t m_replayEnd = 0;
  std::mutex m_sockMutex;
  std::thread m_io;
  std::thread m_parser;

  // PTY bytes the reader queued for the parser; m_inEof once the PTY closed.
  std::mutex m_inMutex;
  std::condition_variable m_inReady;
  std::vector<std::uint8_t> m_inbound;
  bool m_inEof = false;
  // Held: the wait status the holder reported, set with m_inEof.
  std::optional<int> m_heldStatus;

  std::mutex m_outMutex;
  std::vector<std::uint8_t> m_outbound;

  std::atomic<SessionState> m_state{SessionState::Idle};
  std::atomic<int> m_exitStatus{-1};
  std::atomic<bool> m_wakePending{false};
  std::atomic<bool> m_renderHeld{false};
  std::atomic<bool> m_stopping{false};
  std::atomic<bool> m_snapshotWanted{false};
  std::atomic<std::uint64_t> m_bytesRead{0};
  std::atomic<std::uint64_t> m_savedOffset{0};
};

} // namespace term
