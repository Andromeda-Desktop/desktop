#include "pty_session.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace term {

std::optional<SessionState> transition(SessionState from, SessionEvent event) {
  switch (event) {
  case SessionEvent::Spawned:
    if (from == SessionState::Idle) return SessionState::Running;
    return std::nullopt;
  case SessionEvent::ChildExited:
    if (from == SessionState::Running) return SessionState::Exited;
    return std::nullopt;
  }
  return std::nullopt;
}

namespace {

// Bytes the IO thread feeds the parser per lock; bounds how long the UI waits for the lock.
constexpr std::size_t kReadChunk = 64 * 1024;

winsize makeWinsize(std::uint16_t cols, std::uint16_t rows, std::uint16_t cw, std::uint16_t ch) {
  winsize ws{};
  ws.ws_col = cols;
  ws.ws_row = rows;
  ws.ws_xpixel = static_cast<unsigned short>(cols * cw);
  ws.ws_ypixel = static_cast<unsigned short>(rows * ch);
  return ws;
}

} // namespace

PtySession::PtySession(WakeFn wake, MarkFn onMark) : m_wake(std::move(wake)), m_onMark(std::move(onMark)) {}

PtySession::~PtySession() {
  m_stopping = true;
  if (m_eventFd >= 0) {
    std::uint64_t one = 1;
    (void)::write(m_eventFd, &one, sizeof one);
  }
  if (m_io.joinable()) m_io.join();
  if (m_pid > 0 && m_state.load() == SessionState::Running) {
    ::kill(m_pid, SIGHUP);
    ::waitpid(m_pid, nullptr, 0);
  }
  if (m_master >= 0) ::close(m_master);
  if (m_eventFd >= 0) ::close(m_eventFd);
  if (m_terminal != nullptr) ghostty_terminal_free(m_terminal);
}

void PtySession::apply(SessionEvent event) {
  auto next = transition(m_state.load(), event);
  if (next) m_state = *next;
}

bool PtySession::spawn(const SpawnOptions& o) {
  if (m_state.load() != SessionState::Idle || o.argv.empty()) return false;
  if (ghostty_terminal_new(nullptr, &m_terminal, o.cols, o.rows) != GHOSTTY_SUCCESS) return false;
  ghostty_terminal_resize(m_terminal, o.cols, o.rows, o.cellWidthPx, o.cellHeightPx);
  ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_USERDATA, this);
  ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY, reinterpret_cast<const void*>(&onWritePty));
  ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_RENDER_HOLD, reinterpret_cast<const void*>(&onRenderHold));
  ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SEMANTIC_PROMPT,
                       reinterpret_cast<const void*>(&onSemanticPrompt));

  std::vector<char*> argv;
  for (const auto& a : o.argv) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);

  winsize ws = makeWinsize(o.cols, o.rows, o.cellWidthPx, o.cellHeightPx);
  int master = -1;
  pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
  if (pid < 0) return false;
  if (pid == 0) {
    if (!o.cwd.empty() && ::chdir(o.cwd.c_str()) != 0) _exit(127);
    ::setenv("TERM", "xterm-256color", 1);
    ::setenv("COLORTERM", "truecolor", 1);
    ::setenv("TERM_PROGRAM", "andromeda-terminal", 1);
    for (const auto& kv : o.env) ::putenv(const_cast<char*>(kv.c_str()));
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    ::execvp(argv[0], argv.data());
    _exit(127);
  }
  m_pid = pid;
  m_master = master;
  ::fcntl(m_master, F_SETFL, ::fcntl(m_master, F_GETFL) | O_NONBLOCK);
  ::fcntl(m_master, F_SETFD, FD_CLOEXEC);
  m_eventFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  apply(SessionEvent::Spawned);
  m_io = std::thread([this] { ioLoop(); });
  return true;
}

void PtySession::write(std::span<const std::uint8_t> bytes) {
  if (bytes.empty() || m_state.load() != SessionState::Running) return;
  queueOutbound(bytes);
  std::uint64_t one = 1;
  (void)::write(m_eventFd, &one, sizeof one);
}

void PtySession::queueOutbound(std::span<const std::uint8_t> bytes) {
  std::lock_guard lock(m_outMutex);
  m_outbound.insert(m_outbound.end(), bytes.begin(), bytes.end());
}

void PtySession::resize(std::uint16_t cols, std::uint16_t rows, std::uint16_t cw, std::uint16_t ch) {
  {
    std::lock_guard lock(m_termMutex);
    if (m_terminal != nullptr) ghostty_terminal_resize(m_terminal, cols, rows, cw, ch);
  }
  if (m_master >= 0) {
    winsize ws = makeWinsize(cols, rows, cw, ch);
    ::ioctl(m_master, TIOCSWINSZ, &ws);
  }
  signalWake();
}

std::optional<int> PtySession::exitStatus() const {
  if (m_state.load() != SessionState::Exited) return std::nullopt;
  return m_exitStatus.load();
}

void PtySession::signalWake() {
  if (!m_wakePending.exchange(true) && m_wake) m_wake();
}

void PtySession::flushOutbound() {
  std::lock_guard lock(m_outMutex);
  std::size_t done = 0;
  while (done < m_outbound.size()) {
    ssize_t n = ::write(m_master, m_outbound.data() + done, m_outbound.size() - done);
    if (n > 0) {
      done += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    break;
  }
  m_outbound.erase(m_outbound.begin(), m_outbound.begin() + static_cast<std::ptrdiff_t>(done));
}

void PtySession::ioLoop() {
  std::vector<std::uint8_t> buf(kReadChunk);
  for (;;) {
    bool wantOut;
    {
      std::lock_guard lock(m_outMutex);
      wantOut = !m_outbound.empty();
    }
    pollfd fds[2] = {{m_master, static_cast<short>(POLLIN | (wantOut ? POLLOUT : 0)), 0}, {m_eventFd, POLLIN, 0}};
    if (::poll(fds, 2, -1) < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (fds[1].revents & POLLIN) {
      std::uint64_t v;
      (void)::read(m_eventFd, &v, sizeof v);
      if (m_stopping) return;
    }
    if (fds[0].revents & POLLOUT || fds[1].revents & POLLIN) flushOutbound();
    if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
      ssize_t n = ::read(m_master, buf.data(), buf.size());
      if (n > 0) {
        {
          std::lock_guard lock(m_termMutex);
          ghostty_terminal_vt_write(m_terminal, buf.data(), static_cast<std::size_t>(n));
        }
        m_bytesRead += static_cast<std::uint64_t>(n);
        flushOutbound();
        if (!m_renderHeld.load()) signalWake();
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
      int status = 0;
      ::waitpid(m_pid, &status, 0);
      m_exitStatus = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      apply(SessionEvent::ChildExited);
      signalWake();
      return;
    }
  }
}

void PtySession::onWritePty(GhosttyTerminal, void* self, const std::uint8_t* data, std::size_t len) {
  static_cast<PtySession*>(self)->queueOutbound({data, len});
}

void PtySession::onRenderHold(GhosttyTerminal, void* self, bool held) {
  auto* s = static_cast<PtySession*>(self);
  s->m_renderHeld = held;
  if (!held) s->signalWake();
}

void PtySession::onSemanticPrompt(GhosttyTerminal, void* self, const GhosttyTerminalSemanticPrompt* e) {
  auto* s = static_cast<PtySession*>(self);
  if (!s->m_onMark) return;
  CommandMark mark{};
  switch (e->kind) {
  case GHOSTTY_SEMANTIC_PROMPT_PROMPT_START: mark.kind = CommandMark::Kind::PromptStart; break;
  case GHOSTTY_SEMANTIC_PROMPT_INPUT_START: mark.kind = CommandMark::Kind::InputStart; break;
  case GHOSTTY_SEMANTIC_PROMPT_OUTPUT_START: mark.kind = CommandMark::Kind::OutputStart; break;
  case GHOSTTY_SEMANTIC_PROMPT_COMMAND_END: mark.kind = CommandMark::Kind::CommandEnd; break;
  default: return;
  }
  if (e->has_exit_code) mark.exitCode = e->exit_code;
  s->m_onMark(mark);
}

} // namespace term
