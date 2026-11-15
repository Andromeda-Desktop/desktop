#pragma once

#include <optional>

namespace xdpu {
  // Mode of one live thumbnail. Configuring: waits for buffer constraints. Capturing: one frame is in flight.
  // Waiting: the pacing timer runs. Stopped: the source is gone or cannot be captured; no event leaves it.
  enum class LiveState { Configuring, Capturing, Waiting, Stopped };

  enum class LiveEvent {
    Configured,         // new constraints arrived and a buffer was allocated for them
    ConfigureFailed,    // constraints unusable, or the buffer allocation failed
    FrameReady,         // the in-flight frame was copied
    Retry,              // the in-flight frame failed and can be tried again
    ConstraintsChanged, // the in-flight frame failed; new constraints follow
    TimerFired,
    SessionStopped,
  };

  // Side effects the owner runs after it stores `next`, in this order.
  struct LiveStep {
    LiveState next;
    bool cancelTimer = false;   // the pending timer must not fire
    bool dropFrame = false;     // the in-flight frame is abandoned
    bool startTimer = false;
    bool startCapture = false;
  };

  // Empty when `event` cannot happen in `state` (the owner ignores it).
  inline std::optional<LiveStep> liveTransition(LiveState state, LiveEvent event) {
    if (state == LiveState::Stopped) {
      return LiveStep{.next = LiveState::Stopped};
    }
    const bool waiting = state == LiveState::Waiting;
    const bool capturing = state == LiveState::Capturing;
    switch (event) {
    case LiveEvent::Configured:
      return LiveStep{.next = LiveState::Capturing, .cancelTimer = waiting, .dropFrame = capturing, .startCapture = true};
    case LiveEvent::ConfigureFailed:
    case LiveEvent::SessionStopped:
      return LiveStep{.next = LiveState::Stopped, .cancelTimer = waiting, .dropFrame = capturing};
    case LiveEvent::FrameReady:
    case LiveEvent::Retry:
      if (!capturing) {
        return std::nullopt;
      }
      return LiveStep{.next = LiveState::Waiting, .startTimer = true};
    case LiveEvent::ConstraintsChanged:
      if (!capturing) {
        return std::nullopt;
      }
      return LiveStep{.next = LiveState::Configuring};
    case LiveEvent::TimerFired:
      if (!waiting) {
        return std::nullopt;
      }
      return LiveStep{.next = LiveState::Capturing, .startCapture = true};
    }
    return std::nullopt;
  }
} // namespace xdpu
