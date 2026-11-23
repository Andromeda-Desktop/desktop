#pragma once

#include <cstdint>
#include <optional>

namespace andromeda {

  // The session's one activity mode. RestartLocked, Locked and Sleeping are the lock's phases: RestartLocked waits for
  // a lock client after a compositor restart (it owns the escape timer), Sleeping is a locked session in suspend.
  // Output power is a per-output fact beside it and coexists with every phase.
  enum class SessionActivity : uint8_t { Active, Idle, RestartLocked, Locked, Sleeping };

  enum class SessionEvent : uint8_t {
    Input,       // user input or an idle inhibitor
    IdleTimeout, // no input for ScreenActivityLog::kIdleAfter
    RestartLock, // a restarted compositor found the lock file of a locked session
    Lock,        // a lock client presents, or the compositor locks behind its blank
    Unlock,
    Sleep,       // suspend starts with the session locked
    Resume,
  };

  constexpr bool locked(SessionActivity activity) {
    return activity == SessionActivity::RestartLocked || activity == SessionActivity::Locked
        || activity == SessionActivity::Sleeping;
  }

  // The one transition rule. Input and IdleTimeout leave a locked session as it is; a lock event that does not fit the
  // phase returns nullopt, so the session never unlocks from suspend or from a restart lock without a lock client.
  constexpr std::optional<SessionActivity> nextActivity(SessionActivity from, SessionEvent event) {
    using A = SessionActivity;
    switch (event) {
    case SessionEvent::Input:
      return from == A::Idle ? A::Active : from;
    case SessionEvent::IdleTimeout:
      return from == A::Active ? A::Idle : from;
    case SessionEvent::RestartLock:
      return locked(from) ? std::nullopt : std::optional{A::RestartLocked};
    case SessionEvent::Lock:
      return from == A::Locked || from == A::Sleeping ? std::nullopt : std::optional{A::Locked};
    case SessionEvent::Unlock:
      return from == A::Locked ? std::optional{A::Active} : std::nullopt;
    case SessionEvent::Sleep:
      return from == A::Locked ? std::optional{A::Sleeping} : std::nullopt;
    case SessionEvent::Resume:
      return from == A::Sleeping ? std::optional{A::Locked} : std::nullopt;
    }
    return std::nullopt;
  }

} // namespace andromeda
