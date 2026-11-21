#pragma once

#include <cstdint>

// Active: the app runs with windows, or starts or serves without one. Hidden: its last window closed and it stays
// alive, so a reopen skips the process start. Quitting is the loop's state (AppLoop::quitting).
enum class Lifecycle : std::uint8_t { Active, Hidden };
enum class LifecycleEvent : std::uint8_t {
  WindowOpened,
  LastWindowClosed, // only for an app that quits with its last window
  Expired,          // the hidden time ran out, or the system ran short of memory
};
enum class LifecycleStep : std::uint8_t { Ignore, Hide, Show, Quit };

[[nodiscard]] constexpr LifecycleStep lifecycleStep(Lifecycle from, LifecycleEvent event, bool hideWhenClosed) noexcept {
  switch (event) {
  case LifecycleEvent::WindowOpened:
    return from == Lifecycle::Hidden ? LifecycleStep::Show : LifecycleStep::Ignore;
  case LifecycleEvent::LastWindowClosed:
    if (from == Lifecycle::Hidden) {
      return LifecycleStep::Ignore;
    }
    return hideWhenClosed ? LifecycleStep::Hide : LifecycleStep::Quit;
  case LifecycleEvent::Expired:
    return from == Lifecycle::Hidden ? LifecycleStep::Quit : LifecycleStep::Ignore;
  }
  return LifecycleStep::Ignore;
}
