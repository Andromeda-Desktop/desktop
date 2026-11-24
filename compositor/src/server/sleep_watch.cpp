#include "server/sleep_watch.h"
#include "protocols/bus_names.h"

#include "core/log.h"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <systemd/sd-bus.h>
#include <unistd.h>
#include <wayland-server-core.h>

namespace andromeda {

  namespace {
    constexpr Logger kLog("sleep");
    constexpr const char* kLogind = "org.freedesktop.login1";
    constexpr const char* kLogindPath = "/org/freedesktop/login1";
    constexpr const char* kLogindManager = "org.freedesktop.login1.Manager";
    constexpr const char* kSystem1 = busname::kSystem1;
    constexpr const char* kLoginPath = "/org/andromeda/System1/Login";
    constexpr const char* kLogin = "org.andromeda.System1.Login";
    // System1 releases its delay this long before logind's InhibitDelayMaxUSec; the own lock uses the same budget.
    constexpr std::uint64_t kMarginUs = 500'000;
    constexpr std::uint64_t kLateLeadUs = 1'000'000;
  } // namespace

  SleepWatch::SleepWatch(wl_event_loop* loop, std::function<void(bool)> sleeping, std::function<void()> late)
      : m_sleeping(std::move(sleeping)), m_late(std::move(late)), m_bus(loop, bus::Kind::System) {
    if (m_bus.get() == nullptr) {
      kLog.warn("no system bus: the watchdog stays armed across sleep");
    }
    m_lateTimer = loop != nullptr ? wl_event_loop_add_timer(loop, onLate, this) : nullptr;
    m_bus.onChange([this](sd_bus* bus) {
      if (bus != nullptr) {
        follow();
      }
    });
    if (m_bus.get() != nullptr) {
      follow();
    }
  }

  void SleepWatch::follow() {
    sd_bus* bus = m_bus.get();
    if (m_lateTimer == nullptr
        || sd_bus_match_signal_async(
               bus, m_bus.keep(), kLogind, kLogindPath, kLogindManager, "PrepareForSleep", onLogindPrepare, nullptr,
               this
           ) < 0
        || sd_bus_match_signal_async(
               bus, m_bus.keep(), kSystem1, kLoginPath, kLogin, "PrepareForSleep", onSystem1Prepare, nullptr, this
           ) < 0
        || sd_bus_add_match_async(
               bus, m_bus.keep(),
               "type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.andromeda.System1'",
               onOwnerChanged, nullptr, this
           ) < 0) {
      kLog.warn("cannot follow PrepareForSleep");
      return;
    }
    holdThroughSystem1();
    m_bus.sent();
  }

  SleepWatch::~SleepWatch() {
    release();
    m_holdSlot = sd_bus_slot_unref(m_holdSlot);
    m_inhibitSlot = sd_bus_slot_unref(m_inhibitSlot);
    m_budgetSlot = sd_bus_slot_unref(m_budgetSlot);
    if (m_lateTimer != nullptr) {
      wl_event_source_remove(m_lateTimer);
    }
  }

  void SleepWatch::ready() {
    if (m_hold != Hold::Waiting) {
      return;
    }
    m_hold = Hold::Released;
    wl_event_source_timer_update(m_lateTimer, 0);
    kLog.info("ready for sleep");
    if (m_relay == Relay::System1) {
      sd_bus_message* message = nullptr;
      if (sd_bus_message_new_method_call(m_bus.get(), &message, kSystem1, kLoginPath, kLogin, "SleepReady") < 0
          || sd_bus_message_set_expect_reply(message, 0) < 0
          || sd_bus_send(m_bus.get(), message, nullptr) < 0) {
        kLog.warn("cannot send SleepReady");
      }
      sd_bus_message_unref(message);
      m_bus.sent();
    } else {
      release();
    }
  }

  void SleepWatch::holdThroughSystem1() {
    m_holdSlot = sd_bus_slot_unref(m_holdSlot);
    if (sd_bus_call_method_async(
            m_bus.get(), &m_holdSlot, kSystem1, kLoginPath, kLogin, "HoldSleep", onHoldReply, this, ""
        )
        < 0) {
      holdThroughLogind();
    }
  }

  // A System1 that answers HoldSleep holds the only logind delay; without it the own lock takes its place.
  int SleepWatch::onHoldReply(sd_bus_message* reply, void* data, sd_bus_error* /*error*/) {
    auto* self = static_cast<SleepWatch*>(data);
    self->m_holdSlot = sd_bus_slot_unref(self->m_holdSlot);
    std::uint64_t budget = 0;
    if (sd_bus_message_is_method_error(reply, nullptr) || sd_bus_message_read(reply, "t", &budget) < 0) {
      kLog.info(
          "System1 holds no sleep for this session: {}",
          sd_bus_message_get_error(reply) != nullptr ? sd_bus_message_get_error(reply)->name : "bad reply"
      );
      self->holdThroughLogind();
      return 0;
    }
    self->m_relay = Relay::System1;
    self->m_budgetUs = budget;
    self->m_inhibitSlot = sd_bus_slot_unref(self->m_inhibitSlot);
    self->m_budgetSlot = sd_bus_slot_unref(self->m_budgetSlot);
    self->release();
    return 0;
  }

  void SleepWatch::holdThroughLogind() {
    m_relay = Relay::Logind;
    if (m_hold == Hold::Awake) {
      inhibit();
    }
    m_budgetSlot = sd_bus_slot_unref(m_budgetSlot);
    if (sd_bus_call_method_async(
            m_bus.get(), &m_budgetSlot, kLogind, kLogindPath, "org.freedesktop.DBus.Properties", "Get", onBudgetReply,
            this, "ss", kLogindManager, "InhibitDelayMaxUSec"
        )
        < 0) {
      kLog.warn("cannot read logind's InhibitDelayMaxUSec");
    }
  }

  int SleepWatch::onBudgetReply(sd_bus_message* reply, void* data, sd_bus_error* /*error*/) {
    auto* self = static_cast<SleepWatch*>(data);
    self->m_budgetSlot = sd_bus_slot_unref(self->m_budgetSlot);
    std::uint64_t max = 0;
    if (!sd_bus_message_is_method_error(reply, nullptr) && sd_bus_message_read(reply, "v", "t", &max) >= 0) {
      self->m_budgetUs = max > kMarginUs ? max - kMarginUs : 0;
    }
    return 0;
  }

  void SleepWatch::inhibit() {
    m_inhibitSlot = sd_bus_slot_unref(m_inhibitSlot);
    if (sd_bus_call_method_async(
            m_bus.get(), &m_inhibitSlot, kLogind, kLogindPath, kLogindManager, "Inhibit", onInhibitReply, this, "ssss",
            "sleep", "andromeda", "Lock the session before sleep", "delay"
        )
        < 0) {
      kLog.warn("cannot request a sleep delay lock");
    }
  }

  void SleepWatch::release() {
    if (m_lockFd >= 0) {
      close(m_lockFd);
      m_lockFd = -1;
    }
  }

  int SleepWatch::onInhibitReply(sd_bus_message* reply, void* data, sd_bus_error* /*error*/) {
    auto* self = static_cast<SleepWatch*>(data);
    self->m_inhibitSlot = sd_bus_slot_unref(self->m_inhibitSlot);
    int fd = -1;
    if (sd_bus_message_is_method_error(reply, nullptr) || sd_bus_message_read(reply, "h", &fd) < 0) {
      kLog.warn("logind refused a sleep delay lock");
      return 0;
    }
    self->release();
    if (self->m_relay == Relay::Logind && self->m_hold == Hold::Awake) {
      self->m_lockFd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    }
    return 0;
  }

  // NameOwnerChanged (s name, s old owner, s new owner): a new System1 knows no holders; a gone one holds nothing.
  int SleepWatch::onOwnerChanged(sd_bus_message* message, void* data, sd_bus_error* /*error*/) {
    auto* self = static_cast<SleepWatch*>(data);
    const char* name = nullptr;
    const char* oldOwner = nullptr;
    const char* newOwner = nullptr;
    if (sd_bus_message_read(message, "sss", &name, &oldOwner, &newOwner) < 0 || std::strcmp(name, kSystem1) != 0) {
      return 0;
    }
    if (*newOwner != '\0') {
      self->holdThroughSystem1();
    } else if (self->m_relay == Relay::System1) {
      self->holdThroughLogind();
    }
    return 0;
  }

  int SleepWatch::onLogindPrepare(sd_bus_message* message, void* data, sd_bus_error* /*error*/) {
    static_cast<SleepWatch*>(data)->prepare(Relay::Logind, message);
    return 0;
  }

  int SleepWatch::onSystem1Prepare(sd_bus_message* message, void* data, sd_bus_error* /*error*/) {
    static_cast<SleepWatch*>(data)->prepare(Relay::System1, message);
    return 0;
  }

  void SleepWatch::prepare(Relay from, sd_bus_message* message) {
    int start = 0;
    if (from != m_relay || sd_bus_message_read(message, "b", &start) < 0) {
      return;
    }
    if (start != 0) {
      m_hold = Hold::Waiting;
      const std::uint64_t lateMs = (m_budgetUs > kLateLeadUs ? m_budgetUs - kLateLeadUs : 0) / 1000;
      wl_event_source_timer_update(m_lateTimer, static_cast<int>(std::max<std::uint64_t>(lateMs, 1)));
      m_sleeping(true);
      return;
    }
    m_hold = Hold::Awake;
    wl_event_source_timer_update(m_lateTimer, 0);
    m_sleeping(false);
    if (m_relay == Relay::Logind) {
      inhibit();
    }
  }

  int SleepWatch::onLate(void* data) {
    auto* self = static_cast<SleepWatch*>(data);
    if (self->m_hold == Hold::Waiting) {
      kLog.warn("sleep hold budget nearly spent before the session was ready: blanking and locking");
      self->m_late();
      self->ready();
    }
    return 0;
  }

} // namespace andromeda
