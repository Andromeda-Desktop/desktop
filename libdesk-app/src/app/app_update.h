#pragma once

#include <cstdint>

// The app's self-update (macOS bundles). Idle: no helper runs; the 24 h timer starts the next check. Checking: one
// helper child checks and stages. Ready: a verified update waits in the stage; the header shows Restart.
// Swapping: the swap helper starts; it reports SwapStarted or SwapFailed at once. HandedOff: the swap helper owns
// the stage, or the app quits without one; final.
enum class UpdatePhase : std::uint8_t { Idle, Checking, Ready, Swapping, HandedOff };
enum class UpdateEvent : std::uint8_t {
  CheckDue,     // app start and every 24 h
  CheckedReady, // the helper printed `ready <version>`
  CheckedNone,  // `none`, a failure or unparsable output
  Restart,      // the user clicked Restart and every window allows the close
  Quit,         // the app ends (loop end or a session logout)
  SwapStarted,  // the swap helper runs
  SwapFailed,   // the swap helper did not start; the stage stays ready
};
enum class UpdateAction : std::uint8_t { None, SpawnCheck, ShowReady, SwapAndReopen, SwapOnExit, StopCheck };

struct UpdateStep {
  UpdatePhase next;
  UpdateAction action;
};

[[nodiscard]] constexpr UpdateStep updateStep(UpdatePhase from, UpdateEvent event) noexcept {
  const UpdateStep ignore{from, UpdateAction::None};
  if (from == UpdatePhase::HandedOff) {
    return ignore;
  }
  if (from == UpdatePhase::Swapping) {
    switch (event) {
    case UpdateEvent::SwapStarted:
      return {UpdatePhase::HandedOff, UpdateAction::None};
    case UpdateEvent::SwapFailed:
      return {UpdatePhase::Ready, UpdateAction::None};
    default:
      return ignore;
    }
  }
  switch (event) {
  case UpdateEvent::CheckDue:
    return from == UpdatePhase::Idle ? UpdateStep{UpdatePhase::Checking, UpdateAction::SpawnCheck} : ignore;
  case UpdateEvent::CheckedReady:
    return from == UpdatePhase::Checking ? UpdateStep{UpdatePhase::Ready, UpdateAction::ShowReady} : ignore;
  case UpdateEvent::CheckedNone:
    return from == UpdatePhase::Checking ? UpdateStep{UpdatePhase::Idle, UpdateAction::None} : ignore;
  case UpdateEvent::Restart:
    return from == UpdatePhase::Ready ? UpdateStep{UpdatePhase::Swapping, UpdateAction::SwapAndReopen} : ignore;
  case UpdateEvent::Quit:
    switch (from) {
    case UpdatePhase::Ready:
      return {UpdatePhase::Swapping, UpdateAction::SwapOnExit};
    case UpdatePhase::Checking:
      return {UpdatePhase::HandedOff, UpdateAction::StopCheck};
    default:
      return {UpdatePhase::HandedOff, UpdateAction::None};
    }
  case UpdateEvent::SwapStarted:
  case UpdateEvent::SwapFailed:
    return ignore;
  }
  return ignore;
}
