#pragma once

#include <cstdint>
#include <functional>

struct sd_bus;
struct sd_bus_error;
struct sd_bus_message;
struct sd_bus_slot;
struct wl_event_loop;
struct wl_event_source;

namespace andromeda {

  // Holds each sleep until the session is ready for it: through System1 (Login.HoldSleep; System1 holds the only
  // logind delay), or without System1 through an own logind delay lock. `sleeping(true)` runs before the system
  // sleeps; ready() ends the hold. When the hold budget has 1 s left, `late()` runs and the hold ends.
  // Without a system bus it does nothing.
  class SleepWatch {
  public:
    SleepWatch(wl_event_loop* loop, std::function<void(bool sleeping)> sleeping, std::function<void()> late);
    ~SleepWatch();

    SleepWatch(const SleepWatch&) = delete;
    SleepWatch& operator=(const SleepWatch&) = delete;

    // The session may sleep now (locked, or the shell said so). Does nothing unless a sleep waits for it.
    void ready();

  private:
    // Who sends PrepareForSleep and takes the hold.
    enum class Relay : std::uint8_t { None, Logind, System1 };
    // Awake; Waiting: a sleep waits for ready(); Released: the hold ended for this sleep.
    enum class Hold : std::uint8_t { Awake, Waiting, Released };

    static int onEvent(int fd, std::uint32_t mask, void* data);
    static int onLate(void* data);
    static int onLogindPrepare(sd_bus_message* message, void* data, sd_bus_error* error);
    static int onSystem1Prepare(sd_bus_message* message, void* data, sd_bus_error* error);
    static int onOwnerChanged(sd_bus_message* message, void* data, sd_bus_error* error);
    static int onHoldReply(sd_bus_message* reply, void* data, sd_bus_error* error);
    static int onInhibitReply(sd_bus_message* reply, void* data, sd_bus_error* error);
    static int onBudgetReply(sd_bus_message* reply, void* data, sd_bus_error* error);
    void prepare(Relay from, sd_bus_message* message);
    void holdThroughSystem1();
    void holdThroughLogind();
    void inhibit();
    void release();
    void dispatch();

    std::function<void(bool)> m_sleeping;
    std::function<void()> m_late;
    sd_bus* m_bus = nullptr;
    sd_bus_slot* m_holdSlot = nullptr;
    sd_bus_slot* m_inhibitSlot = nullptr;
    sd_bus_slot* m_budgetSlot = nullptr;
    wl_event_source* m_source = nullptr;
    wl_event_source* m_lateTimer = nullptr;
    Relay m_relay = Relay::None;
    Hold m_hold = Hold::Awake;
    // logind's InhibitDelayMaxUSec less System1's 0.5 s margin; logind's default until an answer comes.
    std::uint64_t m_budgetUs = 4'500'000;
    // The own logind delay lock while m_relay is Logind and no sleep runs.
    int m_lockFd = -1;
  };

} // namespace andromeda
