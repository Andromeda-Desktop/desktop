// Drives random event sequences through liveTransition and checks that timers and frames match the mode.
#include "share_picker/live_state.h"

#include <cstdio>
#include <random>

using xdpu::LiveEvent;
using xdpu::LiveState;

int main() {
  std::mt19937 random(20261115);
  std::uniform_int_distribution<int> pick(0, static_cast<int>(LiveEvent::SessionStopped));
  int failures = 0;
  auto check = [&](bool ok, const char* what, int run, int step) {
    if (!ok) {
      std::fprintf(stderr, "run %d step %d: %s\n", run, step, what);
      ++failures;
    }
  };
  for (int run = 0; run < 2000 && failures == 0; ++run) {
    LiveState state = LiveState::Configuring;
    int timers = 0;
    int frames = 0;
    for (int step = 0; step < 60 && failures == 0; ++step) {
      const auto event = static_cast<LiveEvent>(pick(random));
      const bool endsFrame =
          event == LiveEvent::FrameReady || event == LiveEvent::Retry || event == LiveEvent::ConstraintsChanged;
      // A frame result needs a frame in flight; a timer fires only while one is pending.
      const bool possible = state == LiveState::Stopped || (endsFrame ? frames == 1 : event != LiveEvent::TimerFired || timers == 1);
      const auto next = xdpu::liveTransition(state, event);
      check(next.has_value() == possible, "accepts exactly the possible events", run, step);
      if (!next) {
        continue;
      }
      if (state == LiveState::Stopped) {
        check(next->next == LiveState::Stopped && !next->startTimer && !next->startCapture, "Stopped is final", run, step);
      }
      if (state != LiveState::Stopped && endsFrame) {
        --frames;
      }
      if (state != LiveState::Stopped && event == LiveEvent::TimerFired) {
        --timers;
      }
      // The owner keeps a deadline only in Waiting: leaving it drops the pending timer.
      if (state == LiveState::Waiting && event != LiveEvent::TimerFired && next->next != LiveState::Waiting) {
        --timers;
      }
      if (next->dropFrame) {
        check(frames == 1, "drops only an in-flight frame", run, step);
        --frames;
      }
      state = next->next;
      timers += next->startTimer ? 1 : 0;
      frames += next->startCapture ? 1 : 0;
      check(timers == (state == LiveState::Waiting ? 1 : 0), "a timer is pending exactly in Waiting", run, step);
      check(frames == (state == LiveState::Capturing ? 1 : 0), "a frame is in flight exactly in Capturing", run, step);
    }
  }
  return failures == 0 ? 0 : 1;
}
