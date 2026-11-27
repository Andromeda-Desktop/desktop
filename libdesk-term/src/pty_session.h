#pragma once

#include "command_index.h"
#include "osc1337.h"
#include "osc66.h"
#include "sixel.h"
#include "osc99.h"
#include "core/sys/sys.h"

#include <ghostty/vt.h>

#include <atomic>
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

struct SpawnOptions {
  std::vector<std::string> argv;
  std::string cwd;
  std::vector<std::string> env;
  std::uint16_t cols = 80;
  std::uint16_t rows = 24;
  std::uint16_t cellWidthPx = 8;
  std::uint16_t cellHeightPx = 16;
};

// One shell or program on a PTY. One IO thread reads the PTY and feeds the terminal, so each output burst wakes one
// thread; the kernel pty buffer or the holder buffers output during a parse. The UI thread reads terminal state through
// withTerminal() once per frame.
// The pty lives in andromeda-ptyd when it is reachable, so the program outlives a crash of this process; the IO
// thread then reads the holder socket, and keys and window size still go straight to a dup of the pty master.
class PtySession {
public:
  // Called on the IO thread when new state is ready, at most once until takeWake().
  using WakeFn = std::function<void()>;
  // A process group other than the shell's session holds the terminal: a job runs in the foreground.
  [[nodiscard]] bool foregroundJob() const;
  using MarkFn = std::function<void(const CommandMark&)>;
  // IO thread, under the terminal lock; once until bellHandled(), so a burst of BEL rings once.
  using BellFn = std::function<void()>;
  // IO thread, when the program starts or stops reading a secret: canonical input with echo off (a password prompt).
  using SecretFn = std::function<void(bool)>;
  // Program requests from the output stream, on the IO thread under the terminal lock. Replayed output (reattach)
  // sets the title again but never writes the clipboard or notifies.
  struct Effects {
    std::function<void(std::string title)> title;                 // OSC 0 / 2
    std::function<void(std::string text, bool primary)> clipboard; // OSC 52 write, text/plain only
    std::function<std::optional<std::string>(bool primary)> clipboardRead; // OSC 52 read; nullopt denies it
    std::function<void(Notice notice)> notify;       // OSC 9 / 777 / 99
    std::function<void(std::string key)> closeNotice; // OSC 99 p=close
  };

  PtySession(WakeFn wake, MarkFn onMark = {}, BellFn onBell = {}, SecretFn onSecret = {}, Effects effects = {});
  void bellHandled() { m_bellPending = false; } // UI thread: the next BEL calls onBell again
  ~PtySession();
  PtySession(const PtySession&) = delete;
  PtySession& operator=(const PtySession&) = delete;

  bool spawn(const SpawnOptions& options);
  // Takes over an orphaned holder session (holder::listOrphans) and restores its screen.
  bool attach(std::uint64_t id, std::uint16_t cols, std::uint16_t rows, std::uint16_t cellWidthPx,
              std::uint16_t cellHeightPx);
  // The holder session id, or nullopt when the pty is local.
  std::optional<std::uint64_t> heldId() const;
  // A held session outlives this object: the holder keeps its shell with no client. UI thread only.
  void detach() { m_end = End::Detach; }
  void write(std::span<const std::uint8_t> bytes);
  void resize(std::uint16_t cols, std::uint16_t rows, std::uint16_t cellWidthPx, std::uint16_t cellHeightPx);

  // Clears the wake latch; call at the start of the frame that consumes the state.
  bool takeWake() { return m_wakePending.exchange(false); }
  // True while the program holds rendering (synchronized output, mode 2026).
  bool renderHeld() const { return m_renderHeld.load(); }

  template <typename F> auto withTerminal(F&& f) {
    UiLock lock(*this);
    return f(m_terminal);
  }
  // f(terminal, commands) under the terminal lock; entries that left the terminal are gone.
  template <typename F> auto withCommands(F&& f) {
    UiLock lock(*this);
    m_commands.prune();
    return f(m_terminal, m_commands);
  }

  SessionState state() const { return m_state.load(); }
  std::optional<int> exitStatus() const;
  std::uint64_t bytesRead() const { return m_bytesRead.load(); }
  // Output offset of the last snapshot sent to the holder: a reattach replays only the output after it.
  std::uint64_t savedOffset() const { return m_savedOffset.load(); }

private:
  struct LocalChild {
    pid_t pid;
    // Polls readable when the shell exits; a background job can hold the pty open long after that.
    int exitFd;
  };
  struct HeldSession {
    int socket;
    std::uint64_t id;
    // An older holder drops the connection on Detach and relaunches the terminal for the orphan.
    bool detachable;
  };

  bool newTerminal(std::uint16_t cols, std::uint16_t rows, std::uint16_t cellWidthPx, std::uint16_t cellHeightPx);
  void configureTerminal();
  bool spawnHeld(const SpawnOptions& options);
  bool spawnLocal(const SpawnOptions& options);
  void start();
  void ioLoop();
  // Returns at EOF with the holder's wait status (held), or nullopt (local).
  std::optional<int> readHeld(const HeldSession& held);
  std::optional<int> readLocal(const LocalChild& local);
  void parse(std::span<const std::uint8_t> output);
  void finish(std::optional<int> heldStatus);
  // False when the terminal cannot be encoded yet (inside an escape sequence longer than kContinuationBytes).
  bool sendSnapshot(const HeldSession& held);
  void signalWake();
  void flushOutbound();
  void queueOutbound(std::span<const std::uint8_t> bytes);
  void apply(SessionEvent event);

  static void onWritePty(GhosttyTerminal, void* self, const std::uint8_t* data, std::size_t len);
  static void onRenderHold(GhosttyTerminal, void* self, bool held);
  static void onSemanticPrompt(GhosttyTerminal, void* self, const GhosttyTerminalSemanticPrompt* event);
  static void onBellRung(GhosttyTerminal, void* self);
  static GhosttyString onXtversion(GhosttyTerminal, void* self);
  static bool onDeviceAttributes(GhosttyTerminal, void* self, GhosttyDeviceAttributes* out);
  static void onTitleChanged(GhosttyTerminal, void* self);
  static void onClipboardWrite(GhosttyTerminal, void* self, const GhosttyClipboardWrite* write);
  static void onClipboardRead(GhosttyTerminal, void* self, const GhosttyClipboardRead* read);
  static void onNotification(GhosttyTerminal, void* self, const GhosttyTerminalDesktopNotification* note);
  bool replaying() const { return m_parsed < m_replayEnd; }
  void resizeTerminal(std::uint16_t cols, std::uint16_t rows, std::uint16_t cw, std::uint16_t ch);

  WakeFn m_wake;
  MarkFn m_onMark;
  BellFn m_onBell;
  SecretFn m_onSecret;
  Effects m_effects;
  bool m_secret = false; // IO thread
  std::atomic<bool> m_bellPending{false};
  GhosttyTerminal m_terminal = nullptr;
  std::mutex m_termMutex;
  // UI threads waiting for m_termMutex; the IO thread lets them in before it locks again (std::mutex is not fair).
  std::atomic<int> m_uiWaiting{0};
  struct UiLock {
    explicit UiLock(PtySession& s) {
      s.m_uiWaiting.fetch_add(1);
      lock = std::unique_lock(s.m_termMutex);
      s.m_uiWaiting.fetch_sub(1);
      s.m_uiWaiting.notify_all();
    }
    std::unique_lock<std::mutex> lock;
  };
  CommandIndex m_commands; // under m_termMutex

  // Set before the threads start and fixed after.
  std::variant<std::monostate, LocalChild, HeldSession> m_link;
  // What the destructor does to a held session.
  enum class End : std::uint8_t { Close, Detach };
  End m_end = End::Close;
  // Local: the pty master. Held: a dup of the holder's master, for writes and TIOCSWINSZ only.
  int m_master = -1;
  sys::WakeFd m_ioWake;
  // Holder socket bytes not yet decoded; the IO thread's after start().
  std::vector<std::uint8_t> m_sockBuf;
  // Output offsets, IO thread after start().
  std::uint64_t m_received = 0;
  std::uint64_t m_parsed = 0;
  // Attach: the holder's output after its snapshot, parsed first.
  std::vector<std::uint8_t> m_tail;
  // Output before this offset is a replay after attach: the program got the terminal's replies to it already.
  std::uint64_t m_replayEnd = 0;
  Osc99Scanner m_osc99;
  OscFrame m_userVar{"1337;SetUserVar=", 1024}; // IO thread; git_branch=<base64> from the shell integration
  SixelFilter m_sixel;                 // IO thread
  std::vector<std::uint8_t> m_unsixeled; // IO thread: one chunk after m_sixel
  ImageFilter m_image;                 // IO thread
  std::vector<std::uint8_t> m_unimaged; // IO thread: one chunk after m_sixel and m_image
  TextSizeFilter m_textSize;           // IO thread
  std::vector<std::uint8_t> m_filtered; // IO thread: one chunk after all three filters
  std::atomic<std::uint64_t> m_grid{0}; // cols, rows, cell width, cell height: 16 bits each, from the last resize
  std::mutex m_sockMutex;
  std::thread m_io;

  std::mutex m_outMutex;
  std::vector<std::uint8_t> m_outbound;

  std::atomic<SessionState> m_state{SessionState::Idle};
  std::atomic<int> m_exitStatus{-1};
  std::atomic<bool> m_wakePending{false};
  std::atomic<bool> m_renderHeld{false};
  std::atomic<bool> m_stopping{false};
  std::atomic<bool> m_snapshotWanted{false};
  bool m_snapshotRetry = false; // IO thread: a wanted snapshot failed; try again after the next output
  std::atomic<std::uint64_t> m_bytesRead{0};
  std::atomic<std::uint64_t> m_savedOffset{0};
};

} // namespace term
