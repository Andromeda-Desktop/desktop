#pragma once

#include "core/timer_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>

// Caps animation-driven repaints to a fixed rate regardless of the compositor's
// frame-callback rate. Animated widgets (e.g. the scrolling system graphs)
// repaint from the per-frame tick, which fires at the display's refresh rate; on
// high-refresh, high-resolution displays that repaints a large blurred surface
// far more often than the motion needs, burning GPU/CPU for no visible gain.
//
// Gate the per-frame work through shouldStep(): it returns true at most once per
// interval. When it returns false it arms a one-shot timer that re-requests a
// redraw near the interval edge, so the animation still advances at the capped
// rate even after the frame loop would otherwise go idle (the loop is sustained
// by requestRedraw(), so a skipped frame that issues no redraw stops it).
class FrameRateLimiter {
public:
  // Open: the next shouldStep may step. Reviving: a step was refused inside the interval and the revive timer is
  // armed; it returns to Open when the timer fires or on reset().
  enum class Mode : std::uint8_t { Open, Reviving };

  explicit FrameRateLimiter(std::chrono::milliseconds interval = std::chrono::milliseconds{33})
      : m_interval(interval) {}

  bool shouldStep(std::function<void()> reviveCb) {
    const auto now = std::chrono::steady_clock::now();
    const auto since = now - m_lastStepAt;
    if (since >= m_interval) {
      m_lastStepAt = now;
      return true;
    }
    if (m_mode == Mode::Open) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(m_interval - since);
      m_mode = Mode::Reviving;
      m_revive.start(std::max(remaining, std::chrono::milliseconds{1}), [this, cb = std::move(reviveCb)] {
        m_mode = Mode::Open;
        cb();
      });
    }
    return false;
  }

  void reset() {
    m_lastStepAt = {};
    m_mode = Mode::Open;
    m_revive.stop();
  }

  [[nodiscard]] Mode mode() const noexcept { return m_mode; }

private:
  std::chrono::milliseconds m_interval;
  std::chrono::steady_clock::time_point m_lastStepAt;
  Mode m_mode = Mode::Open;
  Timer m_revive;
};
