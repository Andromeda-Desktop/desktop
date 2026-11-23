#pragma once

#include "dbus/sensor/presence_service.h"

#include <chrono>
#include <cstdint>
#include <optional>

// Lock-on-leave and wake-on-approach from the presence sensor. Pure: the caller feeds events and applies the
// effects, the claim (claimed()), the dim (dimmed()) and the timer (timerDelay(), restarted when Effects::rearm).
class PresenceWatch {
public:
  enum class Mode : std::uint8_t {
    Off,      // no claim
    Watching, // claimed: unlocked, screen on, nobody reported away
    Leaving,  // reported away; waits kDebounce of absence and input idle
    Away,     // dimmed; with Leave::Lock waits leaveDelay, then locks
    Locking,  // dimmed; lock requested
    Waking,   // screen off, nobody away yet: an approach needs an away reading first; ends after kWakeWindow
    Approach, // screen off after an away reading: wakes it on a present reading; ends after kWakeWindow
  };
  enum class Leave : std::uint8_t { Off, Dim, Lock };
  struct Settings {
    Leave onLeave = Leave::Off;
    bool wakeOnApproach = false;
    std::int64_t leaveDelaySeconds = 30;
    bool operator==(const Settings&) const = default;
  };
  struct Effects {
    bool lock = false;
    bool wake = false;
    bool rearm = false;
    bool dimChanged = false;
  };

  static constexpr std::int64_t kDebounceSeconds = 5;
  static constexpr std::int64_t kWakeWindowSeconds = 60;

  Effects setSettings(const Settings& settings);
  Effects setLocked(bool locked);
  Effects setScreenOn(bool on);
  Effects reading(Presence presence);
  Effects activity();
  // The timer from timerDelay() ran out; idleSeconds is the session's input idle time, 0 while inhibited.
  Effects timer(std::int64_t idleSeconds);

  [[nodiscard]] Mode mode() const noexcept { return m_mode; }
  [[nodiscard]] bool claimed() const noexcept { return m_mode != Mode::Off; }
  [[nodiscard]] bool dimmed() const noexcept { return m_mode == Mode::Away || m_mode == Mode::Locking; }
  [[nodiscard]] std::optional<std::chrono::seconds> timerDelay() const;

private:
  [[nodiscard]] Mode watchMode() const;
  void transition(Mode to, Effects& effects);
  Effects settle();
  [[nodiscard]] bool waking() const noexcept { return m_mode == Mode::Waking || m_mode == Mode::Approach; }

  Settings m_settings;
  bool m_locked = false;
  bool m_screenOn = true;
  Presence m_presence = Presence::Unknown;
  Mode m_mode = Mode::Off;
};
