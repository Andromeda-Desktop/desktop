#pragma once

#include "core/timer_manager.h"

#include <chrono>
#include <functional>
#include <optional>
#include <vector>

class ShownTimer;

// The repeating UI ticks of one window. They run only while the window is shown (surface presence Shown); hidden,
// occluded, minimized or closed, they wait without wakeups.
class ShownTimers {
public:
  ShownTimers() = default;
  ~ShownTimers();
  ShownTimers(const ShownTimers&) = delete;
  ShownTimers& operator=(const ShownTimers&) = delete;

  // On a change to shown, each started timer ticks once at once, then repeats.
  void setShown(bool shown);
  [[nodiscard]] bool shown() const noexcept { return m_shown; }

private:
  friend class ShownTimer;
  std::vector<ShownTimer*> m_timers;
  bool m_shown = false;
};

// A repeating tick of a window's content (a clock, an elapsed time): Timer's API, gated by ShownTimers.
class ShownTimer {
public:
  ShownTimer() = default;
  ~ShownTimer() { stop(); }
  ShownTimer(const ShownTimer&) = delete;
  ShownTimer& operator=(const ShownTimer&) = delete;

  void startRepeating(ShownTimers& window, std::chrono::milliseconds interval, std::function<void()> callback);
  void stop();
  // Started, whether or not the window is shown now.
  [[nodiscard]] bool active() const noexcept { return m_tick.has_value(); }
  // Started and the window is shown: the tick is due on the loop.
  [[nodiscard]] bool ticking() const noexcept { return m_timer.active(); }

private:
  friend class ShownTimers;
  struct Tick {
    ShownTimers* window = nullptr;
    std::chrono::milliseconds interval{0};
    std::function<void()> callback;
  };
  void run();
  void fire();

  std::optional<Tick> m_tick;
  Timer m_timer;
};
