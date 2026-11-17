#pragma once

#include <cstdint>
#include <functional>
#include <wayland-server-core.h>

namespace andromeda {

  // Turns a stream of change notifications into paced emissions. The first change after a quiet interval emits on the
  // next idle pass; changes during an interval merge into one emission when it ends, so the final state always goes out.
  class EventPacer {
  public:
    EventPacer(wl_event_loop* loop, int intervalMs, std::function<void()> emit);
    ~EventPacer();

    EventPacer(const EventPacer&) = delete;
    EventPacer& operator=(const EventPacer&) = delete;

    void schedule();

  private:
    // Quiet: nothing pending. Queued: an idle pass emits. Interval: an emission went out within intervalMs.
    // IntervalChanged: a change arrived during the interval and emits when it ends.
    enum class Phase : uint8_t { Quiet, Queued, Interval, IntervalChanged };

    static void onIdle(void* data);
    static int onIntervalEnd(void* data);

    void emit();

    wl_event_loop* m_loop;
    int m_intervalMs;
    std::function<void()> m_emit;
    wl_event_source* m_idle = nullptr;
    wl_event_source* m_interval = nullptr;
    Phase m_phase = Phase::Quiet;
  };

} // namespace andromeda
