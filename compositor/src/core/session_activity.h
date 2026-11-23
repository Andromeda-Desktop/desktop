#pragma once

#include <cstdint>
#include <optional>

namespace andromeda {

  // The session's one activity mode. RestartLocked, Locked and SleepingLocked are the lock's phases: RestartLocked
  // waits for a lock client after a compositor restart (it owns the escape timer). Sleeping is a suspend without a lock
  // (lock before suspend is off, or the lock comes late). Output power is a per-output fact beside it.
  enum class SessionActivity : uint8_t { Active, Idle, RestartLocked, Locked, Sleeping, SleepingLocked };

  enum class SessionEvent : uint8_t {
    Input,       // user input or an idle inhibitor
    IdleTimeout, // no input for ScreenActivityLog::kIdleAfter
    RestartLock, // a restarted compositor found the lock file of a locked session
    Lock,        // a lock client presents, or the compositor locks behind its blank
    Unlock,
    Sleep,       // logind PrepareForSleep(true)
    Resume,      // logind PrepareForSleep(false): resume, or an aborted suspend
  };

  constexpr bool locked(SessionActivity activity) {
    return activity == SessionActivity::RestartLocked || activity == SessionActivity::Locked
        || activity == SessionActivity::SleepingLocked;
  }

  constexpr bool sleeping(SessionActivity activity) {
    return activity == SessionActivity::Sleeping || activity == SessionActivity::SleepingLocked;
  }

  // The one transition rule. Input and IdleTimeout change only an unlocked, awake session; Sleep and Resume come from
  // logind and may repeat. A lock event that does not fit the phase returns nullopt, so the session never unlocks
  // in suspend or from a restart lock without a lock client. A restart lock keeps its phase across a suspend.
  constexpr std::optional<SessionActivity> nextActivity(SessionActivity from, SessionEvent event) {
    using A = SessionActivity;
    switch (event) {
    case SessionEvent::Input:
      return from == A::Idle ? A::Active : from;
    case SessionEvent::IdleTimeout:
      return from == A::Active ? A::Idle : from;
    case SessionEvent::RestartLock:
      return from == A::Active || from == A::Idle ? std::optional{A::RestartLocked} : std::nullopt;
    case SessionEvent::Lock:
      if (from == A::Sleeping) {
        return A::SleepingLocked;
      }
      return from == A::Locked || from == A::SleepingLocked ? std::nullopt : std::optional{A::Locked};
    case SessionEvent::Unlock:
      return from == A::Locked ? std::optional{A::Active} : std::nullopt;
    case SessionEvent::Sleep:
      if (from == A::Locked) {
        return A::SleepingLocked;
      }
      return from == A::Active || from == A::Idle ? A::Sleeping : from;
    case SessionEvent::Resume:
      if (from == A::SleepingLocked) {
        return A::Locked;
      }
      return from == A::Sleeping ? A::Active : from;
    }
    return std::nullopt;
  }

} // namespace andromeda
