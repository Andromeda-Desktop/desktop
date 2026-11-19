#include <format>
#include "holder_client.h"
#include "pty_session.h"
#include "workspace_watcher.h"

#include <cstdlib>
#include <filesystem>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <random>
#include <mutex>
#include <string>
#include <vector>

using namespace term;

namespace {

int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

struct Waker {
  std::mutex m;
  std::condition_variable cv;
  bool woke = false;
  void wake() { std::lock_guard l(m); woke = true; cv.notify_all(); }
  // Waits for wakes until pred holds or 10 s pass.
  template <typename P> bool waitFor(PtySession& s, P pred) {
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
      s.takeWake();
      if (pred()) return true;
      std::unique_lock l(m);
      if (!cv.wait_until(l, end, [&] { return woke; })) return pred();
      woke = false;
    }
  }
};

std::string screenText(PtySession& s) {
  return s.withTerminal([](GhosttyTerminal t) {
    GhosttyRenderState rs{};
    GhosttyRenderStateRowIterator it{};
    GhosttyRenderStateRowCells cells{};
    ghostty_render_state_new(nullptr, &rs);
    ghostty_render_state_update(rs, t);
    ghostty_render_state_row_iterator_new(nullptr, &it);
    ghostty_render_state_row_cells_new(nullptr, &cells);
    ghostty_render_state_get(rs, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &it);
    std::string out;
    while (ghostty_render_state_row_iterator_next(it)) {
      ghostty_render_state_row_get(it, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &cells);
      while (ghostty_render_state_row_cells_next(cells)) {
        std::uint8_t b[16];
        GhosttyBuffer buf{b, sizeof b, 0};
        ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &buf);
        out.append(buf.len ? std::string(reinterpret_cast<char*>(b), buf.len) : " ");
      }
      out += '\n';
    }
    ghostty_render_state_row_cells_free(cells);
    ghostty_render_state_row_iterator_free(it);
    ghostty_render_state_free(rs);
    return out;
  });
}

void transitionsKeepInvariants() {
  std::mt19937 rng(7);
  for (int run = 0; run < 2000; ++run) {
    SessionState s = SessionState::Idle;
    bool spawned = false;
    for (int i = 0; i < 12; ++i) {
      auto e = static_cast<SessionEvent>(rng() % 2);
      auto next = transition(s, e);
      if (!next) continue;
      if (s == SessionState::Exited) CHECK(false);
      if (*next == SessionState::Exited) CHECK(spawned);
      if (e == SessionEvent::Spawned) { CHECK(!spawned); spawned = true; }
      s = *next;
    }
  }
}

void runsCommandAndReportsExit() {
  Waker w;
  PtySession s([&] { w.wake(); });
  CHECK(s.spawn({.argv = {"/bin/sh", "-c", "printf 'hello andromeda'; exit 3"}}));
  CHECK(w.waitFor(s, [&] { return s.state() == SessionState::Exited; }));
  CHECK(s.exitStatus() == 3);
  CHECK(screenText(s).find("hello andromeda") != std::string::npos);
}

void answersDeviceStatusReport() {
  // The program asks for the cursor position; the reply must come back through the PTY.
  Waker w;
  PtySession s([&] { w.wake(); });
  CHECK(s.spawn({.argv = {"/bin/bash", "-c",
                          "stty -echo; printf 'ab\\e[6n'; IFS= read -rsd R r; printf '|%s|' \"${r#*[}\"; exit 0"}}));
  CHECK(w.waitFor(s, [&] { return s.state() == SessionState::Exited; }));
  CHECK(screenText(s).find("|1;3|") != std::string::npos);
}

void keyInputReachesProgram() {
  Waker w;
  PtySession s([&] { w.wake(); });
  CHECK(s.spawn({.argv = {"/bin/bash", "-c", "IFS= read -r line; printf 'got:%s' \"$line\"; exit 0"}}));
  std::string in = "typed\r";
  s.write({reinterpret_cast<const std::uint8_t*>(in.data()), in.size()});
  CHECK(w.waitFor(s, [&] { return s.state() == SessionState::Exited; }));
  CHECK(screenText(s).find("got:typed") != std::string::npos);
}

std::string vtText(GhosttyTerminal t) {
  GhosttyFormatter f = nullptr;
  ghostty_formatter_terminal_new(nullptr, &f, t);
  GhosttyFormatterFormat emit = GHOSTTY_FORMATTER_FORMAT_VT;
  ghostty_formatter_set(f, GHOSTTY_FORMATTER_OPT_EMIT, &emit);
  std::uint8_t* p = nullptr;
  std::size_t n = 0;
  ghostty_formatter_format_alloc(f, nullptr, &p, &n);
  std::string out(reinterpret_cast<char*>(p), n);
  ghostty_free(nullptr, p, n);
  ghostty_formatter_free(f);
  return out;
}

GhosttyTerminalScreen activeScreen(GhosttyTerminal t) {
  GhosttyTerminalScreen screen{};
  ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &screen);
  return screen;
}

// A closed window detaches its held shells: they keep running, crash recovery does not list them, and an
// explicit attach takes them over with their input working.
void detachedSessionOutlivesItsWindow() {
  std::uint64_t id = 0;
  {
    Waker w;
    PtySession s([&] { w.wake(); });
    CHECK(s.spawn({.argv = {"/bin/cat"}}) && s.heldId().has_value());
    if (!s.heldId()) return;
    id = *s.heldId();
    s.detach();
  }
  const auto orphans = holder::listOrphans();
  CHECK(std::find(orphans.begin(), orphans.end(), id) == orphans.end());
  Waker w;
  PtySession s([&] { w.wake(); });
  CHECK(s.attach(id, 80, 24, 8, 16));
  s.write({reinterpret_cast<const std::uint8_t*>("alive\n"), 6});
  CHECK(w.waitFor(s, [&] { return screenText(s).find("alive") != std::string::npos; }));
}

// The terminal process dies while a full-screen program sits mid escape sequence on the alternate screen.
// A new process reattaches: its screen must equal a terminal that parsed the whole stream in one go, and the
// program must not get a second reply to a query it sent before the crash. `snapshotted`: the holder keeps a
// snapshot at the crash point (--snapshot-at 1), else the reattach replays all output.
void reattachRestoresScreenMidEscape(bool snapshotted) {
  const std::string before = "\033[6n\033[?1049h\033[2J\033[HTOP\r\n\033[3";
  const std::string after = "1mRED<x>";
  const std::string script = "stty -echo; printf '\\033[6n'; IFS= read -rsd R r; "
                             "printf '\\033[?1049h\\033[2J\\033[HTOP\\n\\033[3'; IFS= read -r x; "
                             "printf '1mRED<%q>' \"$x\"; exec cat";
  int ids[2];
  CHECK(::pipe(ids) == 0);
  const pid_t crashing = ::fork();
  if (crashing == 0) {
    Waker w;
    auto* s = new PtySession([&] { w.wake(); });
    if (!s->spawn({.argv = {"/bin/bash", "-c", script}}) || !s->heldId()) _exit(2);
    const auto crashPoint = [&] { return (snapshotted ? s->savedOffset() : s->bytesRead()) == before.size(); };
    if (!w.waitFor(*s, crashPoint)) _exit(3);
    const std::uint64_t id = *s->heldId();
    (void)::write(ids[1], &id, sizeof id);
    _exit(0);
  }
  ::close(ids[1]);
  std::uint64_t id = 0;
  CHECK(::read(ids[0], &id, sizeof id) == sizeof id);
  ::close(ids[0]);
  int status = 0;
  ::waitpid(crashing, &status, 0);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  Waker w;
  PtySession s([&] { w.wake(); });
  bool attached = false;
  for (int i = 0; i < 500 && !attached; ++i) {
    // The holder sees the hang-up of the dead process asynchronously.
    const auto orphans = holder::listOrphans();
    attached = std::find(orphans.begin(), orphans.end(), id) != orphans.end() && s.attach(id, 80, 24, 8, 16);
    if (!attached) ::usleep(10'000);
  }
  CHECK(attached);
  if (!attached) return;
  // Type only after the replay: a reply the replay wrongly sends then lands in this line, where %q shows it.
  CHECK(w.waitFor(s, [&] { return s.bytesRead() == (snapshotted ? 0 : before.size()); }));
  s.write({reinterpret_cast<const std::uint8_t*>("x\n"), 2});
  CHECK(w.waitFor(s, [&] { return screenText(s).find("RED<") != std::string::npos; }));

  GhosttyTerminal reference = nullptr;
  ghostty_terminal_new(nullptr, &reference, 80, 24);
  const std::string all = before + after;
  ghostty_terminal_vt_write(reference, reinterpret_cast<const std::uint8_t*>(all.data()), all.size());
  s.withTerminal([&](GhosttyTerminal t) {
    CHECK(activeScreen(t) == GHOSTTY_TERMINAL_SCREEN_ALTERNATE);
    CHECK(vtText(t) == vtText(reference));
    return 0;
  });
  ghostty_terminal_free(reference);
}

void programEffectsReachTheApp() {
  // Title, OSC 52 copy (clipboard and primary), OSC 9 and 777 notifications, and XTVERSION naming the app.
  Waker w;
  std::mutex m;
  std::vector<std::string> got;
  const auto log = [&](std::string line) {
    std::lock_guard lock(m);
    got.push_back(std::move(line));
  };
  PtySession s([&] { w.wake(); }, {}, {}, {},
               {.title = [&](std::string t) { log("title:" + t); },
                .clipboard = [&](std::string t, bool primary) { log((primary ? "primary:" : "clip:") + t); },
                .notify = [&](Notice n) { log("note:" + n.title + "/" + n.body); }});
  CHECK(s.spawn({.argv = {"/bin/bash", "-c", R"(stty -echo
printf '\e]2;my title\a\e]52;c;aGk=\a\e]52;p;eW8=\a\e]9;built\a\e]777;notify;Tests;all pass\a\e[>q'
IFS= read -rsd '\' r; printf '|%s|' "${r:4:18}"; exit 0)"}}));
  CHECK(w.waitFor(s, [&] { return s.state() == SessionState::Exited; }));
  CHECK(screenText(s).find("|Andromeda Terminal|") != std::string::npos);
  std::lock_guard lock(m);
  for (const char* want : {"title:my title", "clip:hi", "primary:yo", "note:/built", "note:Tests/all pass"}) {
    if (!std::ranges::contains(got, std::string(want))) {
      std::fprintf(stderr, "FAIL missing effect %s\n", want);
      CHECK(false);
    }
  }
}

void kittyNotificationsReachTheApp() {
  // OSC 99: title, buttons and a base64 body in chunks of one id with options, a close, and a query that gets a reply.
  Waker w;
  std::mutex m;
  std::vector<std::string> got;
  PtySession s([&] { w.wake(); }, {}, {}, {},
               {.notify =
                    [&](Notice n) {
                      std::string buttons;
                      for (const std::string& b : n.buttons) buttons += "[" + b + "]";
                      std::lock_guard lock(m);
                      got.push_back(std::format("{}:{}/{} u{} f{} r{} o{} {}", n.key, n.title, n.body, n.urgency, n.focus,
                                                n.report, static_cast<int>(n.when), buttons));
                    },
                .closeNotice =
                    [&](std::string key) {
                      std::lock_guard lock(m);
                      got.push_back("close:" + key);
                    }});
  CHECK(s.spawn({.argv = {"/bin/bash", "-c", R"(stty -echo
printf '\e]99;i=b1:d=0:u=2:a=report,-focus;Build\e\\'; printf '\e]99;i=b1:d=0:o=unfocused:p=buttons;Retry\xe2\x80\xa8Open\e\\'
printf '\e]99;i=b1:p=body:e=1;ZG9uZQ==\e\\'; printf '\e]99;i=b1:p=close;\e\\'; printf '\e]99;i=q:p=?;\e\\'
IFS= read -rsd '\' r; printf '|%s|' "${r:1:16}"; exit 0)"}}));
  CHECK(w.waitFor(s, [&] { return s.state() == SessionState::Exited; }));
  CHECK(screenText(s).find("|]99;i=q:p=?;a=fo|") != std::string::npos);
  std::lock_guard lock(m);
  CHECK((got == std::vector<std::string>{"b1:Build/done u2 ffalse rtrue o1 [Retry][Open]", "close:b1"}));
}

void shellBranchReachesThePrompt() {
  // OSC 1337 SetUserVar=git_branch after 133;A names the prompt's branch; a sequence split across writes still counts.
  Waker w;
  PtySession s([&] { w.wake(); });
  CHECK(s.spawn({.argv = {"/bin/bash", "-c", R"(printf '\e]133;A\a\e]1337;SetUser'; sleep 0.1; printf 'Var=git_branch=ZmVhdC/DqS14\a'
printf '\e]1337;SetUserVar=other=eA==\a'; exit 0)"}}));
  CHECK(w.waitFor(s, [&] { return s.state() == SessionState::Exited; }));
  const std::string branch = s.withCommands([](GhosttyTerminal, const CommandIndex& index) {
    return index.entries().empty() ? std::string("<none>") : index.entries().back().branch;
  });
  CHECK(branch == "feat/\xc3\xa9-x");
}

void runSessionTests() {
  shellBranchReachesThePrompt();
  kittyNotificationsReachTheApp();
  runsCommandAndReportsExit();
  programEffectsReachTheApp();
  answersDeviceStatusReport();
  keyInputReachesProgram();
}

// Another client's workspace edit reaches the watcher, and the watcher's own socket never blocks the caller.
void watcherFollowsWorkspaceTable() {
  int changes = 0;
  WorkspaceWatcher watcher([&] { ++changes; });
  CHECK(watcher.connect());
  auto pumpOn = [](WorkspaceWatcher& w, auto pred) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!pred() && std::chrono::steady_clock::now() < end) {
      std::vector<pollfd> fds;
      const std::size_t start = w.addPollFds(fds);
      if (::poll(fds.data(), fds.size(), 100) > 0) w.dispatch(fds, start);
    }
    return pred();
  };
  auto pump = [&](auto pred) { return pumpOn(watcher, pred); };
  CHECK(pump([&] { return changes > 0; }));
  const int editor = holder::connectSocket();
  CHECK(editor >= 0);
  CHECK(holder::sendAll(editor, holder::encodePutWorkspace(1, 0, "web", "/srv/web", "globe")));
  CHECK(pump([&] {
    const auto& ws = watcher.workspaces().workspaces;
    return std::ranges::any_of(ws, [](const holder::WorkspaceInfo& w) { return w.name == "web" && w.root == "/srv/web"; });
  }));
  ::close(editor);

  std::optional<std::uint64_t> made;
  const auto req = watcher.put({.name = "api", .root = "/srv/api"}, [&](std::uint32_t, std::uint64_t id) { made = id; });
  CHECK(req.has_value() && pump([&] { return made.has_value(); }));
  Waker w;
  PtySession s([&] { w.wake(); });
  CHECK(s.spawn({.argv = {"/bin/sleep", "5"}}) && s.heldId().has_value() && made.has_value());
  if (!s.heldId() || !made) return;
  watcher.setSession(*s.heldId(), *made);
  CHECK(pump([&] {
    return std::ranges::any_of(watcher.workspaces().sessions, [&](const holder::SessionInfo& info) {
      return info.id == *s.heldId() && info.workspace == *made;
    });
  }));

  // A new connection closes the workspaces left with no shell ("web") and keeps those with one ("api").
  WorkspaceWatcher second([] {});
  CHECK(second.connect());
  auto count = [&](const std::string& name) {
    return std::ranges::count(second.workspaces().workspaces, name, &holder::WorkspaceInfo::name);
  };
  CHECK(pumpOn(second, [&] { return count("web") == 0 && count("api") == 1; }));
}

} // namespace

int main(int argc, char** argv) {
  transitionsKeepInvariants();
  ::setenv("ANDROMEDA_PTYD_SOCKET", "/nonexistent/ptyd.sock", 1);
  runSessionTests();

  if (argc < 2) {
    std::fprintf(stderr, "FAIL usage: pty_session_test <andromeda-ptyd>\n");
    return 1;
  }
  char dir[] = "/tmp/pty-session-test-XXXXXX";
  CHECK(::mkdtemp(dir) != nullptr);
  for (const bool snapshotted : {true, false}) {
    const std::string sock = std::string(dir) + (snapshotted ? "/snap.sock" : "/tail.sock");
    const pid_t holderPid = ::fork();
    if (holderPid == 0) {
      ::execl(argv[1], argv[1], "--socket", sock.c_str(), "--snapshot-at", snapshotted ? "1" : "1000000", nullptr);
      _exit(127);
    }
    ::setenv("ANDROMEDA_PTYD_SOCKET", sock.c_str(), 1);
    for (int i = 0; i < 500 && !std::filesystem::exists(sock); ++i) ::usleep(10'000);
    CHECK(std::filesystem::exists(sock));
    if (snapshotted) {
      runSessionTests();
      Waker w;
      PtySession s([&] { w.wake(); });
      CHECK(s.spawn({.argv = {"/bin/true"}}) && s.heldId().has_value());
      detachedSessionOutlivesItsWindow();
      watcherFollowsWorkspaceTable();
    }
    reattachRestoresScreenMidEscape(snapshotted);
    ::kill(holderPid, SIGTERM);
    ::waitpid(holderPid, nullptr, 0);
  }
  std::filesystem::remove_all(dir);
  if (failures == 0) std::puts("pty_session: all passed");
  return failures == 0 ? 0 : 1;
}
