#pragma once

#include "core/timer_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>

// The update clock of one plugin script: `started` (the script runs) x `shown` (its host surface presence is
// Shown; a plugin with no surface stays shown), and while both hold, whether a due update waits for the owner's
// deferral (a panel transition). The script's `update` runs every interval only in Running.
enum class PluginUpdateMode : std::uint8_t { StoppedShown, StoppedHidden, Running, Deferred, Paused };

// Due and DueDeferred: the interval or the retry elapsed, and the deferral answered no or yes.
enum class PluginUpdateEvent : std::uint8_t { Start, Stop, Show, Hide, Due, DueDeferred };

// What the owner does after a transition: arm the interval timer, arm the deferral retry, disarm, run the update,
// or ask the deferral at once and apply Due or DueDeferred (the catch-up after a pause).
struct PluginUpdateStep {
  PluginUpdateMode next = PluginUpdateMode::StoppedShown;
  bool arm = false;
  bool retry = false;
  bool disarm = false;
  bool update = false;
  bool due = false;
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
  case M::Deferred:
    if (event == E::Stop) {
      return {.next = M::StoppedShown, .disarm = true};
    }
    if (event == E::Hide) {
      return {.next = M::Paused, .disarm = true};
    }
    if (event == E::DueDeferred) {
      return {.next = M::Deferred, .retry = true};
    }
    if (event == E::Due) {
      return {.next = M::Running, .arm = mode == M::Deferred, .update = true};
    }
    return {.next = mode};
  case M::Paused:
    if (event == E::Stop) {
      return {.next = M::StoppedHidden};
    }
    if (event == E::Show) {
      return {.next = M::Running, .arm = true, .due = true};
    }
    return {.next = M::Paused};
  }
  return {.next = mode};
}

// Spread: the first update after each arm comes a per-timer phase (50 ms steps, at most 500 ms and half the
// interval) after the interval, so many scripts on one surface do not update in the same frame.
enum class PluginUpdatePhase : std::uint8_t { Aligned, Spread };

// The one owner of a plugin's update interval (floor kFloorMs), phase, deferral retry and timer; callers state
// facts: started, shown, interval, and a deferral predicate.
class PluginUpdateTimer {
public:
  static constexpr int kFloorMs = 16;
  static constexpr int kDefaultMs = 1000;
  static constexpr std::chrono::milliseconds kRetry{50};
  static constexpr std::chrono::milliseconds kPhaseStep{50};
  static constexpr std::chrono::milliseconds kMaxPhase{500};

  [[nodiscard]] static constexpr int clampInterval(int ms) noexcept { return std::max(kFloorMs, ms); }

  [[nodiscard]] static constexpr std::chrono::milliseconds firstDelay(int intervalMs, std::uint32_t phase) noexcept {
    const std::chrono::milliseconds interval{intervalMs};
    const auto maxPhaseMs = std::min({interval / 2, kMaxPhase, interval - std::chrono::milliseconds(1)}).count();
    if (maxPhaseMs <= 0) {
      return interval;
    }
    return interval + std::chrono::milliseconds((static_cast<std::int64_t>(phase) * kPhaseStep.count()) % (maxPhaseMs + 1));
  }

  explicit PluginUpdateTimer(std::function<void()> update, PluginUpdatePhase phase = PluginUpdatePhase::Aligned)
      : m_update(std::move(update)), m_phase(phase == PluginUpdatePhase::Spread ? nextPhase() : 0) {}

  void start() { apply(PluginUpdateEvent::Start); }
  void stop() { apply(PluginUpdateEvent::Stop); }
  void setShown(bool shown) { apply(shown ? PluginUpdateEvent::Show : PluginUpdateEvent::Hide); }
  void setDeferral(std::function<bool()> deferNow) { m_deferNow = std::move(deferNow); }

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
  static std::uint32_t nextPhase() {
    static std::uint32_t next = 0;
    return next++;
  }

  void apply(PluginUpdateEvent event) {
    const PluginUpdateStep step = nextPluginUpdate(m_mode, event);
    m_mode = step.next;
    if (step.disarm) {
      m_timer.stop();
    }
    if (step.arm) {
      arm();
    }
    if (step.retry) {
      m_timer.start(kRetry, [this] { due(); });
    }
    if (step.update) {
      m_update();
    }
    if (step.due) {
      due();
    }
  }

  void due() { apply(m_deferNow && m_deferNow() ? PluginUpdateEvent::DueDeferred : PluginUpdateEvent::Due); }

  // The repeating timer is armed before the update runs, so a retry or disarm from that update replaces it.
  void arm() {
    const auto interval = std::chrono::milliseconds(m_intervalMs);
    const auto first = firstDelay(m_intervalMs, m_phase);
    if (first == interval) {
      m_timer.startRepeating(interval, [this] { due(); });
      return;
    }
    m_timer.start(first, [this, interval] {
      m_timer.startRepeating(interval, [this] { due(); });
      due();
    });
  }

  std::function<void()> m_update;
  std::function<bool()> m_deferNow;
  Timer m_timer;
  PluginUpdateMode m_mode = PluginUpdateMode::StoppedShown;
  int m_intervalMs = kDefaultMs;
  std::uint32_t m_phase = 0;
};
