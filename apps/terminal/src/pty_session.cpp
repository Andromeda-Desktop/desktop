#include "pty_session.h"

#include "holder_client.h"
#include "core/sys/sys.h"
#include <termios.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>
#include <span>
#include <string_view>

#include <cerrno>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ; // POSIX: no header declares it on macOS

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

// Bytes the parser feeds the terminal per lock; bounds how long the UI waits for the lock.
constexpr std::size_t kReadChunk = 64 * 1024;
// The reader stops reading at this backlog, so the kernel blocks the program and Ctrl+C output is not far behind.
constexpr std::size_t kInboundMax = 128 * 1024;
// History limit, as the Ghostty app; the library default keeps a single page.
constexpr std::size_t kScrollbackBytes = 10'000'000;
// An unfinished escape sequence up to this size survives a holder snapshot.
constexpr std::size_t kContinuationBytes = 1u << 20;
// Kitty and sixel images, per screen; libghostty keeps 10 MB, less than one full-window sixel.
constexpr std::uint64_t kImageStorageBytes = 320ull << 20;
// A program killed by the loss of its holder reports as killed by SIGHUP.
constexpr int kHolderLostStatus = 128 + SIGHUP;

int exitCode(int status) { return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status); }

winsize makeWinsize(std::uint16_t cols, std::uint16_t rows, std::uint16_t cw, std::uint16_t ch) {
  winsize ws{};
  ws.ws_col = cols;
  ws.ws_row = rows;
  ws.ws_xpixel = static_cast<unsigned short>(cols * cw);
  ws.ws_ypixel = static_cast<unsigned short>(rows * ch);
  return ws;
}

// True when no ESC in `bytes` starts a DCS or an OSC and none ends it: the sixel and OSC 66 filters and the OSC scanners pass such bytes unchanged and stay idle.
// Bash forwards its SIGHUP to its jobs only when the signal wins a race with the hangup, so a closed session hangs up
// what is left in it itself; nohup'd jobs ignore it, setsid'd ones are in another session.
void hangUpSession(pid_t sid) {
  sys::forEachSessionProcess(sid, [](int pid) {
    ::kill(pid, SIGHUP);
    ::kill(pid, SIGCONT); // a stopped job takes the SIGHUP once it runs
  });
}

bool plainChunk(std::span<const std::uint8_t> bytes) {
  const std::uint8_t* p = bytes.data();
  const std::uint8_t* const end = p + bytes.size();
  while ((p = static_cast<const std::uint8_t*>(std::memchr(p, 0x1b, static_cast<std::size_t>(end - p)))) != nullptr) {
    if (++p == end || *p == 'P' || *p == ']') return false;
  }
  return true;
}

} // namespace

PtySession::PtySession(WakeFn wake, MarkFn onMark, BellFn onBell, SecretFn onSecret, Effects effects)
    : m_wake(std::move(wake)), m_onMark(std::move(onMark)), m_onBell(std::move(onBell)), m_onSecret(std::move(onSecret)),
      m_effects(std::move(effects)) {}

PtySession::~PtySession() {
  m_stopping = true;
  m_ioWake.signal();
  {
    // Under the queue lock, so the parser cannot test m_stopping and then miss this notify.
    std::lock_guard lock(m_inMutex);
  }
  m_inReady.notify_all();
  if (m_io.joinable()) m_io.join();
  if (m_parser.joinable()) m_parser.join();
  if (auto* local = std::get_if<LocalChild>(&m_link)) {
    if (m_state.load() == SessionState::Running) {
      // Closing the master hangs up the whole foreground job: a shell that traps SIGHUP still waits for it.
      ::close(std::exchange(m_master, -1));
      ::kill(local->pid, SIGHUP);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
      while (::waitpid(local->pid, nullptr, WNOHANG) == 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
          ::kill(local->pid, SIGKILL);
          ::waitpid(local->pid, nullptr, 0);
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
    hangUpSession(local->pid);
    if (local->exitFd >= 0) ::close(local->exitFd);
  }
  if (auto* held = std::get_if<HeldSession>(&m_link)) {
    (void)holder::sendAll(held->socket, m_end == End::Detach && held->detachable ? holder::encodeDetach(held->id)
                                                                                 : holder::encodeClose(held->id));
    ::close(held->socket);
  }
  if (m_master >= 0) ::close(m_master);
  if (m_terminal != nullptr) ghostty_terminal_free(m_terminal);
}

void PtySession::apply(SessionEvent event) {
  auto next = transition(m_state.load(), event);
  if (next) m_state = *next;
}

bool PtySession::newTerminal(std::uint16_t cols, std::uint16_t rows, std::uint16_t cw, std::uint16_t ch) {
  if (ghostty_terminal_new(nullptr, &m_terminal, cols, rows) != GHOSTTY_SUCCESS) return false;
  resizeTerminal(cols, rows, cw, ch);
  configureTerminal();
  return true;
}

void PtySession::resizeTerminal(std::uint16_t cols, std::uint16_t rows, std::uint16_t cw, std::uint16_t ch) {
  ghostty_terminal_resize(m_terminal, cols, rows, cw, ch);
  m_grid = std::uint64_t{cols} | std::uint64_t{rows} << 16 | std::uint64_t{cw} << 32 | std::uint64_t{ch} << 48;
}

void PtySession::configureTerminal() {
  ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_USERDATA, this);
  ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, &kScrollbackBytes);
  ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_CONTINUATION_MAX_BYTES, &kContinuationBytes);
  ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT, &kImageStorageBytes);
  ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY, reinterpret_cast<const void*>(&onWritePty));
  ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_RENDER_HOLD, reinterpret_cast<const void*>(&onRenderHold));
  ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SEMANTIC_PROMPT,
                       reinterpret_cast<const void*>(&onSemanticPrompt));
  ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_BELL, reinterpret_cast<const void*>(&onBellRung));
  ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_XTVERSION, reinterpret_cast<const void*>(&onXtversion));
  ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_DEVICE_ATTRIBUTES,
                       reinterpret_cast<const void*>(&onDeviceAttributes));
  ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED, reinterpret_cast<const void*>(&onTitleChanged));
  ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_CLIPBOARD_WRITE,
                       reinterpret_cast<const void*>(&onClipboardWrite));
  ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_CLIPBOARD_READ, reinterpret_cast<const void*>(&onClipboardRead));
  ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_DESKTOP_NOTIFICATION,
                       reinterpret_cast<const void*>(&onNotification));
}

bool PtySession::spawn(const SpawnOptions& o) {
  if (m_state.load() != SessionState::Idle || o.argv.empty()) return false;
  if (!newTerminal(o.cols, o.rows, o.cellWidthPx, o.cellHeightPx)) return false;
  if (!spawnHeld(o) && !spawnLocal(o)) return false;
  start();
  return true;
}

bool PtySession::spawnHeld(const SpawnOptions& o) {
  const int sock = holder::connectSocket();
  if (sock < 0) return false;
  std::vector<std::string> env;
  for (char** e = environ; *e != nullptr; ++e) env.emplace_back(*e);
  env.insert(env.end(), {"TERM=xterm-256color", "COLORTERM=truecolor", "TERM_PROGRAM=andromeda-terminal"});
  env.insert(env.end(), o.env.begin(), o.env.end());
  const winsize ws = makeWinsize(o.cols, o.rows, o.cellWidthPx, o.cellHeightPx);
  const holder::WinSize size{ws.ws_col, ws.ws_row, ws.ws_xpixel, ws.ws_ypixel};
  int fd = -1;
  std::optional<holder::Message> reply;
  if (holder::sendAll(sock, holder::encodeSpawn(1, size, o.cwd, o.argv, env))) {
    reply = holder::receiveOne(sock, m_sockBuf, fd);
  }
  const auto* spawned = reply ? std::get_if<holder::Spawned>(&*reply) : nullptr;
  if (spawned == nullptr || fd < 0) {
    if (fd >= 0) ::close(fd);
    ::close(sock);
    m_sockBuf.clear();
    return false;
  }
  m_master = fd;
  m_link = HeldSession{sock, spawned->id, holder::holderVersion() >= 2};
  return true;
}

bool PtySession::spawnLocal(const SpawnOptions& o) {
  std::vector<char*> argv;
  for (const auto& a : o.argv) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);

  winsize ws = makeWinsize(o.cols, o.rows, o.cellWidthPx, o.cellHeightPx);
  int master = -1;
  pid_t pid = sys::forkPty(&master, &ws);
  if (pid < 0) return false;
  if (pid == 0) {
    if (!o.cwd.empty() && ::chdir(o.cwd.c_str()) != 0) _exit(127);
    ::setenv("TERM", "xterm-256color", 1);
    ::setenv("COLORTERM", "truecolor", 1);
    ::setenv("TERM_PROGRAM", "andromeda-terminal", 1);
    for (const auto& kv : o.env) ::putenv(const_cast<char*>(kv.c_str()));
    // An app started from a script inherits ignored SIGINT/SIGQUIT; the shell must start with defaults.
    for (int sig = 1; sig < NSIG; ++sig) ::signal(sig, SIG_DFL);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    ::execvp(argv[0], argv.data());
    _exit(127);
  }
  m_master = master;
  m_link = LocalChild{pid, sys::childExitFd(pid)};
  return true;
}

bool PtySession::attach(std::uint64_t id, std::uint16_t cols, std::uint16_t rows, std::uint16_t cw,
                        std::uint16_t ch) {
  if (m_state.load() != SessionState::Idle) return false;
  const int sock = holder::connectSocket();
  if (sock < 0) return false;
  int fd = -1;
  std::optional<holder::Message> reply;
  if (holder::sendAll(sock, holder::encodeAttach(id))) reply = holder::receiveOne(sock, m_sockBuf, fd);
  auto* attached = reply ? std::get_if<holder::Attached>(&*reply) : nullptr;
  if (attached == nullptr) {
    ::close(sock);
    m_sockBuf.clear();
    return false;
  }
  // A snapshot from another terminal build may not decode: the tail alone then still shows the latest output.
  bool restored = false;
  if (!attached->snapshot.empty()) {
    GhosttySnapshotDecoder decoder = nullptr;
    if (ghostty_snapshot_decoder_new_buf(nullptr, &decoder, attached->snapshot.data(), attached->snapshot.size()) ==
        GHOSTTY_SUCCESS) {
      ghostty_snapshot_decoder_set(decoder, GHOSTTY_SNAPSHOT_DECODER_OPT_MAX_CONTINUATION_BYTES, &kContinuationBytes);
      const bool retain = true;
      ghostty_snapshot_decoder_set(decoder, GHOSTTY_SNAPSHOT_DECODER_OPT_RETAIN_CONTINUATION, &retain);
      restored = ghostty_snapshot_decoder_decode(decoder, &m_terminal) == GHOSTTY_SUCCESS;
      ghostty_snapshot_decoder_free(decoder);
    }
  }
  if (restored) {
    resizeTerminal(cols, rows, cw, ch);
    configureTerminal();
  } else if (!newTerminal(cols, rows, cw, ch)) {
    if (fd >= 0) ::close(fd);
    ::close(sock);
    return false;
  }
  m_inbound = std::move(attached->tail);
  m_parsed = attached->offset;
  m_received = attached->offset + m_inbound.size();
  m_replayEnd = m_received;
  m_master = fd;
  m_link = HeldSession{sock, id, holder::holderVersion() >= 2};
  if (m_master >= 0) {
    const winsize ws = makeWinsize(cols, rows, cw, ch);
    ::ioctl(m_master, TIOCSWINSZ, &ws);
  }
  start();
  return true;
}

std::optional<std::uint64_t> PtySession::heldId() const {
  if (const auto* held = std::get_if<HeldSession>(&m_link)) return held->id;
  return std::nullopt;
}

void PtySession::start() {
  if (m_master >= 0) {
    ::fcntl(m_master, F_SETFL, ::fcntl(m_master, F_GETFL) | O_NONBLOCK);
    ::fcntl(m_master, F_SETFD, FD_CLOEXEC);
  }
  apply(SessionEvent::Spawned);
  m_io = std::thread([this] { ioLoop(); });
  m_parser = std::thread([this] { parseLoop(); });
}

void PtySession::write(std::span<const std::uint8_t> bytes) {
  if (bytes.empty() || m_state.load() != SessionState::Running) return;
  queueOutbound(bytes);
  m_ioWake.signal();
}

void PtySession::queueOutbound(std::span<const std::uint8_t> bytes) {
  std::lock_guard lock(m_outMutex);
  m_outbound.insert(m_outbound.end(), bytes.begin(), bytes.end());
}

void PtySession::resize(std::uint16_t cols, std::uint16_t rows, std::uint16_t cw, std::uint16_t ch) {
  {
    UiLock lock(*this);
    if (m_terminal != nullptr) resizeTerminal(cols, rows, cw, ch);
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

void PtySession::endInput(std::optional<int> heldStatus) {
  {
    std::lock_guard lock(m_inMutex);
    m_heldStatus = heldStatus;
    m_inEof = true;
  }
  m_inReady.notify_one();
}

void PtySession::ioLoopHeld(const HeldSession& held) {
  std::vector<int> fds;
  for (;;) {
    bool wantOut;
    {
      std::lock_guard lock(m_outMutex);
      wantOut = !m_outbound.empty() && m_master >= 0;
    }
    bool full;
    {
      std::lock_guard lock(m_inMutex);
      full = m_inbound.size() >= kInboundMax;
    }
    // A full queue leaves the socket unread; the holder then stops reading the pty, so the program blocks.
    pollfd pfds[3] = {{full ? -1 : held.socket, POLLIN, 0}, {wantOut ? m_master : -1, POLLOUT, 0}, {m_ioWake.pollFd(), POLLIN, 0}};
    if (::poll(pfds, 3, -1) < 0) {
      if (errno == EINTR) continue;
      return endInput(kHolderLostStatus << 8);
    }
    if (pfds[2].revents & POLLIN) {
      m_ioWake.drain();
      if (m_stopping) return;
    }
    if (pfds[1].revents & POLLOUT || pfds[2].revents & POLLIN) flushOutbound();
    if (full || !(pfds[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
    long n;
    while ((n = holder::receive(held.socket, m_sockBuf, fds, true)) > 0) {
      if (m_sockBuf.size() >= kReadChunk) break;
    }
    for (int fd : fds) ::close(fd);
    fds.clear();
    std::size_t used = 0;
    std::vector<std::uint8_t> output;
    std::optional<int> exited;
    bool broken = n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
    for (;;) {
      std::optional<holder::Message> msg;
      const auto took = holder::decode(std::span(m_sockBuf).subspan(used), msg);
      if (!took) broken = true;
      if (!took || *took == 0) break;
      used += *took;
      if (auto* o = std::get_if<holder::Output>(&*msg)) {
        // A gap means a lost frame: the screen would be wrong from here on.
        if (o->offset != m_received) {
          broken = true;
          break;
        }
        m_received += o->bytes.size();
        output.insert(output.end(), o->bytes.begin(), o->bytes.end());
      } else if (std::holds_alternative<holder::WantSnapshot>(*msg)) {
        m_snapshotWanted = true;
      } else if (auto* e = std::get_if<holder::Exited>(&*msg)) {
        exited = e->status;
      }
    }
    m_sockBuf.erase(m_sockBuf.begin(), m_sockBuf.begin() + static_cast<std::ptrdiff_t>(used));
    {
      std::lock_guard lock(m_inMutex);
      m_inbound.insert(m_inbound.end(), output.begin(), output.end());
    }
    m_inReady.notify_one();
    if (exited) return endInput(exited);
    if (broken) return endInput(kHolderLostStatus << 8);
  }
}

void PtySession::ioLoop() {
  if (const auto* held = std::get_if<HeldSession>(&m_link)) return ioLoopHeld(*held);
  const int exitFd = std::get<LocalChild>(m_link).exitFd;
  std::vector<std::uint8_t> buf(kReadChunk);
  for (;;) {
    bool wantOut;
    {
      std::lock_guard lock(m_outMutex);
      wantOut = !m_outbound.empty();
    }
    bool full;
    {
      std::lock_guard lock(m_inMutex);
      full = m_inbound.size() >= kInboundMax;
    }
    // A full queue leaves the master out of the poll (it would report POLLHUP at once); the parser's wake
    // resumes reading.
    const short events = static_cast<short>((full ? 0 : POLLIN) | (wantOut ? POLLOUT : 0));
    pollfd fds[3] = {{events != 0 ? m_master : -1, events, 0}, {m_ioWake.pollFd(), POLLIN, 0}, {exitFd, POLLIN, 0}};
    if (::poll(fds, 3, -1) < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (fds[1].revents & POLLIN) {
      m_ioWake.drain();
      if (m_stopping) return;
    }
    if (fds[0].revents & POLLOUT || fds[1].revents & POLLIN) flushOutbound();
    if (fds[2].revents & (POLLIN | POLLHUP)) {
      // The shell's last output is queued in the pty now; read only that much, so a job it left behind that
      // floods the pty cannot keep the session open.
      int queued = 0;
      ::ioctl(m_master, FIONREAD, &queued);
      ssize_t n = 0;
      while (queued > 0 && (n = ::read(m_master, buf.data(), buf.size())) > 0) {
        queued -= static_cast<int>(n);
        std::lock_guard lock(m_inMutex);
        m_inbound.insert(m_inbound.end(), buf.begin(), buf.begin() + n);
      }
      m_inReady.notify_one();
      return endInput(std::nullopt);
    }
    if (full || !(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
    // A PTY read returns at most 4 KiB: drain up to a chunk, so a flood takes one queue lock per chunk.
    std::size_t got = 0;
    ssize_t n = 0;
    while (got < buf.size() && (n = ::read(m_master, buf.data() + got, buf.size() - got)) > 0) {
      got += static_cast<std::size_t>(n);
    }
    if (got > 0) {
      {
        std::lock_guard lock(m_inMutex);
        m_inbound.insert(m_inbound.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(got));
      }
      m_inReady.notify_one();
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
    return endInput(std::nullopt);
  }
}

bool PtySession::sendSnapshot(const HeldSession& held) {
  std::uint8_t* blob = nullptr;
  std::size_t len = 0;
  GhosttyResult result;
  {
    std::lock_guard lock(m_termMutex);
    result = ghostty_snapshot_encode_alloc(m_terminal, nullptr, &blob, &len);
  }
  if (result != GHOSTTY_SUCCESS) return false;
  {
    std::lock_guard lock(m_sockMutex);
    if (holder::sendAll(held.socket, holder::encodeSnapshot(held.id, m_parsed, {blob, len}))) m_savedOffset = m_parsed;
  }
  ghostty_free(nullptr, blob, len);
  return true;
}

void PtySession::parseLoop() {
  std::vector<std::uint8_t> work;
  for (;;) {
    bool wasFull;
    {
      std::unique_lock lock(m_inMutex);
      m_inReady.wait(lock, [this] { return m_stopping || m_inEof || !m_inbound.empty() || m_snapshotWanted; });
      if (m_stopping) return;
      if (m_inbound.empty() && m_inEof) break;
      wasFull = m_inbound.size() >= kInboundMax;
      work.clear();
      work.swap(m_inbound);
    }
    if (wasFull) {
      m_ioWake.signal();
    }
    for (std::size_t at = 0, len = 0; at < work.size(); at += len) {
      len = std::min(kReadChunk, work.size() - at);
      if (m_parsed < m_replayEnd) len = std::min<std::size_t>(len, m_replayEnd - m_parsed);
      for (int waiting; (waiting = m_uiWaiting.load()) > 0;) m_uiWaiting.wait(waiting);
      const std::span<const std::uint8_t> chunk{work.data() + at, len};
      const bool plain = m_sixel.idle() && m_image.idle() && m_textSize.idle() && m_osc99.idle() && m_userVar.idle()
                         && plainChunk(chunk);
      if (!plain) {
        m_unsixeled.clear();
        m_sixel.feed(chunk, m_unsixeled);
        const std::uint64_t g = m_grid.load();
        m_unimaged.clear();
        m_image.feed(m_unsixeled, m_unimaged,
                     {static_cast<int>(g & 0xffff), static_cast<int>(g >> 16 & 0xffff), static_cast<int>(g >> 32 & 0xffff),
                      static_cast<int>(g >> 48)});
        m_filtered.clear();
        m_textSize.feed(m_unimaged, m_filtered);
      }
      {
        std::lock_guard lock(m_termMutex);
        if (plain) {
          ghostty_terminal_vt_write(m_terminal, chunk.data(), chunk.size());
        } else {
          ghostty_terminal_vt_write(m_terminal, m_filtered.data(), m_filtered.size());
        }
      }
      if (!plain && !replaying()) {
        m_osc99.feed(chunk, [this](Osc99Scanner::Event e) {
          if (auto* show = std::get_if<Osc99Scanner::Show>(&e); show != nullptr && m_effects.notify) {
            m_effects.notify(std::move(show->notice));
          } else if (auto* close = std::get_if<Osc99Scanner::Close>(&e); close != nullptr && m_effects.closeNotice) {
            m_effects.closeNotice(std::move(close->key));
          } else if (const auto* query = std::get_if<Osc99Scanner::Query>(&e)) {
            const std::string reply = Osc99Scanner::queryReply(query->key);
            queueOutbound({reinterpret_cast<const std::uint8_t*>(reply.data()), reply.size()});
          }
        });
      }
      if (!plain) m_userVar.feed(chunk, [this](std::string_view payload) {
        constexpr std::string_view kBranch = "git_branch=";
        if (!payload.starts_with(kBranch)) return;
        std::lock_guard lock(m_termMutex);
        m_commands.onBranch(base64Decode(payload.substr(kBranch.size())));
      });
      m_bytesRead += len;
      m_parsed += len;
      flushOutbound();
      if (!m_renderHeld.load()) signalWake();
    }
    // A prompt turns echo off before it prints, so a check per output batch sees it without a timer.
    if (termios mode{}; m_onSecret && m_master >= 0 && ::tcgetattr(m_master, &mode) == 0) {
      const bool secret = (mode.c_lflag & ICANON) != 0 && (mode.c_lflag & ECHO) == 0;
      if (secret != m_secret) {
        m_secret = secret;
        m_onSecret(secret);
      }
    }
    if (const auto* held = std::get_if<HeldSession>(&m_link);
        held && (m_snapshotWanted.exchange(false) || m_snapshotRetry)) {
      m_snapshotRetry = !sendSnapshot(*held);
    }
  }
  if (const auto* local = std::get_if<LocalChild>(&m_link)) {
    int status = 0;
    ::waitpid(local->pid, &status, 0);
    m_exitStatus = exitCode(status);
  } else {
    std::lock_guard lock(m_inMutex);
    m_exitStatus = exitCode(m_heldStatus.value_or(kHolderLostStatus << 8));
  }
  apply(SessionEvent::ChildExited);
  signalWake();
}

void PtySession::onWritePty(GhosttyTerminal, void* self, const std::uint8_t* data, std::size_t len) {
  auto* s = static_cast<PtySession*>(self);
  if (s->m_parsed < s->m_replayEnd) return;
  s->queueOutbound({data, len});
}

void PtySession::onRenderHold(GhosttyTerminal, void* self, bool held) {
  auto* s = static_cast<PtySession*>(self);
  s->m_renderHeld = held;
  if (!held) s->signalWake();
}

void PtySession::onBellRung(GhosttyTerminal /*terminal*/, void* self) {
  auto* s = static_cast<PtySession*>(self);
  if (s->m_onBell && !s->m_bellPending.exchange(true)) s->m_onBell();
}

GhosttyString PtySession::onXtversion(GhosttyTerminal, void*) {
  static constexpr std::string_view kVersion = "Andromeda Terminal " ANDROMEDA_TERMINAL_VERSION;
  return {reinterpret_cast<const std::uint8_t*>(kVersion.data()), kVersion.size()};
}

// libghostty's default reply (VT220, ANSI colour) plus sixel graphics.
bool PtySession::onDeviceAttributes(GhosttyTerminal, void*, GhosttyDeviceAttributes* out) {
  out->primary.conformance_level = GHOSTTY_DA_CONFORMANCE_VT220;
  out->primary.features[0] = GHOSTTY_DA_FEATURE_SIXEL;
  out->primary.features[1] = GHOSTTY_DA_FEATURE_ANSI_COLOR;
  out->primary.num_features = 2;
  out->secondary.device_type = GHOSTTY_DA_DEVICE_TYPE_VT220;
  return true;
}

void PtySession::onTitleChanged(GhosttyTerminal terminal, void* self) {
  auto* s = static_cast<PtySession*>(self);
  GhosttyString title{};
  if (!s->m_effects.title || ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_TITLE, &title) != GHOSTTY_SUCCESS) return;
  s->m_effects.title(std::string(reinterpret_cast<const char*>(title.ptr), title.len));
}

void PtySession::onClipboardRead(GhosttyTerminal, void* self, const GhosttyClipboardRead* read) {
  auto* s = static_cast<PtySession*>(self);
  const std::optional<std::string> text = s->m_effects.clipboardRead && !s->replaying()
                                              ? s->m_effects.clipboardRead(read->location != GHOSTTY_CLIPBOARD_LOCATION_STANDARD)
                                              : std::nullopt;
  static constexpr std::string_view kMime = "text/plain";
  GhosttyClipboardContent content{};
  GhosttyClipboardReadReply reply = GHOSTTY_INIT_SIZED(GhosttyClipboardReadReply);
  reply.result = GHOSTTY_CLIPBOARD_READ_RESULT_DENIED;
  if (text) {
    content.mime = {.ptr = reinterpret_cast<const std::uint8_t*>(kMime.data()), .len = kMime.size()};
    content.data = {.ptr = reinterpret_cast<const std::uint8_t*>(text->data()), .len = text->size()};
    reply.result = GHOSTTY_CLIPBOARD_READ_RESULT_SUCCESS;
    reply.contents = &content;
    reply.contents_len = 1;
  }
  read->reply(read, &reply);
}

void PtySession::onClipboardWrite(GhosttyTerminal, void* self, const GhosttyClipboardWrite* write) {
  auto* s = static_cast<PtySession*>(self);
  const std::span contents(write->contents, write->contents_len);
  const auto text = std::ranges::find_if(contents, [](const GhosttyClipboardContent& c) {
    const std::string_view mime(reinterpret_cast<const char*>(c.mime.ptr), c.mime.len);
    return mime == "text/plain" || mime.starts_with("text/plain;");
  });
  GhosttyClipboardWriteReply reply = GHOSTTY_INIT_SIZED(GhosttyClipboardWriteReply);
  reply.result = !s->m_effects.clipboard || s->replaying() ? GHOSTTY_CLIPBOARD_WRITE_RESULT_DENIED
      : text == contents.end()                              ? GHOSTTY_CLIPBOARD_WRITE_RESULT_UNSUPPORTED
                                                            : GHOSTTY_CLIPBOARD_WRITE_RESULT_SUCCESS;
  if (reply.result == GHOSTTY_CLIPBOARD_WRITE_RESULT_SUCCESS) {
    s->m_effects.clipboard(std::string(reinterpret_cast<const char*>(text->data.ptr), text->data.len),
                           write->location != GHOSTTY_CLIPBOARD_LOCATION_STANDARD);
  }
  write->reply(write, &reply);
}

void PtySession::onNotification(GhosttyTerminal, void* self, const GhosttyTerminalDesktopNotification* note) {
  auto* s = static_cast<PtySession*>(self);
  if (!s->m_effects.notify || s->replaying()) return;
  s->m_effects.notify(Notice{.title = std::string(reinterpret_cast<const char*>(note->title.ptr), note->title.len),
                             .body = std::string(reinterpret_cast<const char*>(note->body.ptr), note->body.len)});
}

void PtySession::onSemanticPrompt(GhosttyTerminal terminal, void* self, const GhosttyTerminalSemanticPrompt* e) {
  auto* s = static_cast<PtySession*>(self);
  CommandMark mark{};
  switch (e->kind) {
  case GHOSTTY_SEMANTIC_PROMPT_PROMPT_START: mark.kind = CommandMark::Kind::PromptStart; break;
  case GHOSTTY_SEMANTIC_PROMPT_INPUT_START: mark.kind = CommandMark::Kind::InputStart; break;
  case GHOSTTY_SEMANTIC_PROMPT_OUTPUT_START: mark.kind = CommandMark::Kind::OutputStart; break;
  case GHOSTTY_SEMANTIC_PROMPT_COMMAND_END: mark.kind = CommandMark::Kind::CommandEnd; break;
  default: return;
  }
  if (e->has_exit_code) mark.exitCode = e->exit_code;
  if (e->command.len > 0) mark.command.assign(reinterpret_cast<const char*>(e->command.ptr), e->command.len);
  s->m_commands.onMark(terminal, mark, std::chrono::steady_clock::now());
  if (s->m_onMark) s->m_onMark(mark);
}

bool PtySession::foregroundJob() const {
  if (m_master < 0) {
    return false;
  }
  // On a pty master both calls describe the slave side: its foreground group and its session (the shell).
  const pid_t group = ::tcgetpgrp(m_master);
  const pid_t session = ::tcgetsid(m_master);
  return group > 0 && session > 0 && group != session;
}

} // namespace term
