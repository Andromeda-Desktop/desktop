#include "idle/presence_watch.h"

#include <cassert>

PresenceWatch::Mode PresenceWatch::watchMode() const {
  if (m_settings.onLeave == Leave::Off || m_locked || !m_screenOn) {
    return Mode::Off;
  }
  return m_presence == Presence::Absent ? Mode::Leaving : Mode::Watching;
}

void PresenceWatch::transition(Mode to, Effects& effects) {
  [[maybe_unused]] const bool valid = to == Mode::Off || to == Mode::Watching || to == Mode::Leaving
      || (to == Mode::Away && m_mode == Mode::Leaving) || (to == Mode::Locking && m_mode == Mode::Away)
      || (to == Mode::Waking && m_settings.wakeOnApproach && !m_screenOn)
      || (to == Mode::Approach && m_settings.wakeOnApproach && !m_screenOn && m_presence == Presence::Absent);
  assert(valid);
  if (to != m_mode) {
    const bool wasDimmed = dimmed();
    m_mode = to;
    if (to == Mode::Off) {
      m_presence = Presence::Unknown;
    }
    effects.rearm = true;
    effects.dimChanged = effects.dimChanged != (wasDimmed != dimmed());
  }
}

// Returns to the mode the environment asks for; Away and Locking survive only while their conditions hold.
PresenceWatch::Effects PresenceWatch::settle() {
  Effects effects;
  if (waking()) {
    if (!m_settings.wakeOnApproach || m_screenOn) {
      transition(watchMode(), effects);
    } else if (m_mode == Mode::Waking && m_presence == Presence::Absent) {
      transition(Mode::Approach, effects);
    }
    return effects;
  }
  const Mode want = watchMode();
  const bool holdsAway = (m_mode == Mode::Away || m_mode == Mode::Locking) && want == Mode::Leaving;
  if (!holdsAway && !(m_mode == Mode::Leaving && want == Mode::Leaving)) {
    transition(want, effects);
  }
  return effects;
}

PresenceWatch::Effects PresenceWatch::setSettings(const Settings& settings) {
  m_settings = settings;
  Effects effects = settle();
  effects.rearm = true;
  return effects;
}

PresenceWatch::Effects PresenceWatch::setLocked(bool locked) {
  m_locked = locked;
  return settle();
}

PresenceWatch::Effects PresenceWatch::setScreenOn(bool on) {
  m_screenOn = on;
  if (!on && m_settings.wakeOnApproach && !waking()) {
    Effects effects;
    transition(m_presence == Presence::Absent ? Mode::Approach : Mode::Waking, effects);
    return effects;
  }
  return settle();
}

PresenceWatch::Effects PresenceWatch::reading(Presence presence) {
  if (presence == Presence::Unknown) {
    return {};
  }
  m_presence = presence;
  if (m_mode == Mode::Approach && presence == Presence::Present) {
    Effects effects{.wake = true};
    transition(watchMode(), effects);
    return effects;
  }
  return settle();
}

PresenceWatch::Effects PresenceWatch::activity() {
  if (m_mode != Mode::Leaving && m_mode != Mode::Away && m_mode != Mode::Locking) {
    return {};
  }
  Effects effects;
  transition(Mode::Watching, effects);
  transition(watchMode(), effects);
  effects.rearm = true;
  return effects;
}

PresenceWatch::Effects PresenceWatch::timer(std::int64_t idleSeconds) {
  Effects effects{.rearm = true};
  switch (m_mode) {
  case Mode::Leaving:
    if (idleSeconds >= kDebounceSeconds) {
      transition(Mode::Away, effects);
    }
    break;
  case Mode::Away:
    if (m_settings.onLeave != Leave::Lock) {
      break;
    }
    if (idleSeconds >= kDebounceSeconds + m_settings.leaveDelaySeconds) {
      transition(Mode::Locking, effects);
      effects.lock = true;
    } else {
      transition(Mode::Watching, effects);
      transition(watchMode(), effects);
    }
    break;
  case Mode::Waking:
  case Mode::Approach:
    transition(Mode::Off, effects);
    break;
  case Mode::Off:
  case Mode::Watching:
  case Mode::Locking:
    break;
  }
  return effects;
}

std::optional<std::chrono::seconds> PresenceWatch::timerDelay() const {
  switch (m_mode) {
  case Mode::Leaving:
    return std::chrono::seconds(kDebounceSeconds);
  case Mode::Away:
    if (m_settings.onLeave != Leave::Lock) {
      return std::nullopt;
    }
    return std::chrono::seconds(m_settings.leaveDelaySeconds);
  case Mode::Waking:
  case Mode::Approach:
    return std::chrono::seconds(kWakeWindowSeconds);
  case Mode::Off:
  case Mode::Watching:
  case Mode::Locking:
    return std::nullopt;
  }
  return std::nullopt;
}
