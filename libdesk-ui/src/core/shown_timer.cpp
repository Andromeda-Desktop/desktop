#include "core/shown_timer.h"

#include <algorithm>

ShownTimers::~ShownTimers() {
  for (ShownTimer* timer : m_timers) {
    timer->m_timer.stop();
    timer->m_tick.reset();
  }
}

void ShownTimers::setShown(bool shown) {
  if (shown == m_shown) {
    return;
  }
  m_shown = shown;
  // A tick may stop or start timers: walk a copy, and skip those stopped meanwhile.
  const std::vector<ShownTimer*> timers = m_timers;
  for (ShownTimer* timer : timers) {
    if (std::ranges::find(m_timers, timer) == m_timers.end()) {
      continue;
    }
    if (!shown) {
      timer->m_timer.stop();
      continue;
    }
    timer->run();
    timer->fire();
  }
}

void ShownTimer::startRepeating(ShownTimers& window, std::chrono::milliseconds interval, std::function<void()> callback) {
  stop();
  m_tick = Tick{.window = &window, .interval = interval, .callback = std::move(callback)};
  window.m_timers.push_back(this);
  if (window.shown()) {
    run();
  }
}

void ShownTimer::stop() {
  m_timer.stop();
  if (m_tick) {
    std::erase(m_tick->window->m_timers, this);
    m_tick.reset();
  }
}

void ShownTimer::run() {
  m_timer.startRepeating(m_tick->interval, [this] { fire(); });
}

void ShownTimer::fire() {
  // The callback may stop this timer, which frees m_tick: call a copy.
  const std::function<void()> callback = m_tick->callback;
  callback();
}
