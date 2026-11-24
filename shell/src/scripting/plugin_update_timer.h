#pragma once

#include "core/timer_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>

// The update clock of one plugin script: `started` (the script runs) x `shown` (its host surface presence is
// Shown; a plugin with no surface stays shown). The script's `update` runs every interval only while both hold.
enum class PluginUpdateMode : std::uint8_t { StoppedShown, StoppedHidden, Running, Paused };

enum class PluginUpdateEvent : std::uint8_t { Start, Stop, Show, Hide };

// What the owner does after a transition: arm the repeating timer, disarm it, or run one update at once (the
// catch-up after a pause).
struct PluginUpdateStep {
  PluginUpdateMode next = PluginUpdateMode::StoppedShown;
  bool arm = false;
  bool disarm = false;
  bool updateNow = false;
};

[[nodiscard]] constexpr PluginUpdateStep nextPluginUpdate(PluginUpdateMode mode, PluginUpdateEvent event) noexcept {
  using M = PluginUpdateMode;
  using E = PluginUpdateEvent;
  switch (mode) {
  case M::StoppedShown:
    if (event == E::Start) {
      return {.next = M::Running, .arm = true};
    }
    return {.next = event == E::Hide ? M::StoppedHidden : M::StoppedShown};
  case M::StoppedHidden:
    if (event == E::Start) {
      return {.next = M::Paused};
    }
    return {.next = event == E::Show ? M::StoppedShown : M::StoppedHidden};
  case M::Running:
    if (event == E::Stop) {
      return {.next = M::StoppedShown, .disarm = true};
    }
    if (event == E::Hide) {
      return {.next = M::Paused, .disarm = true};
    }
    return {.next = M::Running};
  case M::Paused:
    if (event == E::Stop) {
      return {.next = M::StoppedHidden};
    }
    if (event == E::Show) {
      return {.next = M::Running, .arm = true, .updateNow = true};
    }
    return {.next = M::Paused};
  }
  return {.next = mode};
}

// The one owner of a plugin's update interval (floor kFloorMs) and timer; callers state facts: started, shown,
// interval.
class PluginUpdateTimer {
public:
  static constexpr int kFloorMs = 16;
  static constexpr int kDefaultMs = 1000;

  [[nodiscard]] static constexpr int clampInterval(int ms) noexcept { return std::max(kFloorMs, ms); }

  explicit PluginUpdateTimer(std::function<void()> update) : m_update(std::move(update)) {}

  void start() { apply(PluginUpdateEvent::Start); }
  void stop() { apply(PluginUpdateEvent::Stop); }
  void setShown(bool shown) { apply(shown ? PluginUpdateEvent::Show : PluginUpdateEvent::Hide); }

  // Rearms a running timer when the clamped interval changed.
  void setInterval(int ms) {
    const int next = clampInterval(ms);
    if (next == m_intervalMs) {
      return;
    }
    m_intervalMs = next;
    if (m_mode == PluginUpdateMode::Running) {
      arm();
    }
  }

  [[nodiscard]] int intervalMs() const noexcept { return m_intervalMs; }
  [[nodiscard]] PluginUpdateMode mode() const noexcept { return m_mode; }

private:
  void apply(PluginUpdateEvent event) {
    const PluginUpdateStep step = nextPluginUpdate(m_mode, event);
    m_mode = step.next;
    if (step.disarm) {
      m_timer.stop();
    }
    if (step.arm) {
      arm();
    }
    if (step.updateNow) {
      m_update();
    }
  }

  void arm() {
    m_timer.startRepeating(std::chrono::milliseconds(m_intervalMs), [this] { m_update(); });
  }

  std::function<void()> m_update;
  Timer m_timer;
  PluginUpdateMode m_mode = PluginUpdateMode::StoppedShown;
  int m_intervalMs = kDefaultMs;
};
