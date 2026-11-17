#include "server/event_pacer.h"

#include "core/log.h"

#include <utility>

namespace andromeda {

  namespace {
    constexpr Logger kLog("ipc");
  } // namespace

  EventPacer::EventPacer(wl_event_loop* loop, int intervalMs, std::function<void()> emit)
      : m_loop(loop), m_intervalMs(intervalMs), m_emit(std::move(emit)),
        m_interval(wl_event_loop_add_timer(loop, onIntervalEnd, this)) {
    if (m_interval == nullptr) {
      kLog.error("failed to register event pacing timer, events will go out unpaced");
    }
  }

  EventPacer::~EventPacer() {
    if (m_idle != nullptr) {
      wl_event_source_remove(m_idle);
    }
    if (m_interval != nullptr) {
      wl_event_source_remove(m_interval);
    }
  }

  void EventPacer::schedule() {
    switch (m_phase) {
      case Phase::Queued:
      case Phase::IntervalChanged:
        return;
      case Phase::Interval:
        m_phase = Phase::IntervalChanged;
        return;
      case Phase::Quiet:
        m_idle = wl_event_loop_add_idle(m_loop, onIdle, this);
        if (m_idle == nullptr) {
          kLog.error("failed to register event pacing idle source, dropping a change notification");
          return;
        }
        m_phase = Phase::Queued;
        return;
    }
  }

  void EventPacer::emit() {
    m_phase = Phase::Quiet;
    if (m_interval != nullptr) {
      if (wl_event_source_timer_update(m_interval, m_intervalMs) == 0) {
        m_phase = Phase::Interval;
      } else {
        kLog.error("failed to arm event pacing timer, this event goes out unpaced");
      }
    }
    m_emit();
  }

  void EventPacer::onIdle(void* data) {
    auto* pacer = static_cast<EventPacer*>(data);
    pacer->m_idle = nullptr;
    pacer->emit();
  }

  int EventPacer::onIntervalEnd(void* data) {
    auto* pacer = static_cast<EventPacer*>(data);
    if (pacer->m_phase == Phase::IntervalChanged) {
      pacer->emit();
    } else {
      pacer->m_phase = Phase::Quiet;
    }
    return 0;
  }

} // namespace andromeda
