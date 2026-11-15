#include "pty_session.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <random>
#include <string>

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

} // namespace

int main() {
  transitionsKeepInvariants();
  runsCommandAndReportsExit();
  answersDeviceStatusReport();
  keyInputReachesProgram();
  if (failures == 0) std::puts("pty_session: all passed");
  return failures == 0 ? 0 : 1;
}
