#pragma once

#include <cstdint>

// Active: the app runs with windows, or starts or serves without one. Hidden: its last window closed and it stays
// alive, so a reopen skips the process start. HiddenExpired: no window, the app wants to quit, and leases (work
// that outlives the windows, such as a file copy) keep it until the last one ends. Quitting: the loop ends; final.
enum class Lifecycle : std::uint8_t { Active, Hidden, HiddenExpired, Quitting };
enum class LifecycleEvent : std::uint8_t {
  WindowOpened,
  LastWindowClosed, // only for an app that quits with its last window
  Expired,          // no window and nothing to wait for: hidden time out, memory pressure, idle service start
  LeaseReleased,    // after the count went down
  Quit,             // signal, session lost, service quit, app.quit: ends the app whatever it holds
};
enum class LifecycleStep : std::uint8_t { Ignore, Hide, Show, Wait, Quit };

struct LifecycleFacts {
  bool hideWhenClosed = false;
  bool windowOpen = false; // after the event
  int leases = 0;          // after the event
};

[[nodiscard]] constexpr Lifecycle lifecycleAfter(LifecycleStep step, Lifecycle from) noexcept {
  switch (step) {
  case LifecycleStep::Ignore:
    return from;
  case LifecycleStep::Hide:
    return Lifecycle::Hidden;
  case LifecycleStep::Show:
    return Lifecycle::Active;
  case LifecycleStep::Wait:
    return Lifecycle::HiddenExpired;
  case LifecycleStep::Quit:
    return Lifecycle::Quitting;
  }
  return from;
}

[[nodiscard]] constexpr LifecycleStep lifecycleStep(Lifecycle from, LifecycleEvent event, LifecycleFacts facts) noexcept {
  if (from == Lifecycle::Quitting) {
    return LifecycleStep::Ignore;
  }
  const LifecycleStep expire = facts.leases > 0 ? LifecycleStep::Wait : LifecycleStep::Quit;
  switch (event) {
  case LifecycleEvent::WindowOpened:
    return from == Lifecycle::Active ? LifecycleStep::Ignore : LifecycleStep::Show;
  case LifecycleEvent::LastWindowClosed:
    if (from != Lifecycle::Active || facts.windowOpen) {
      return LifecycleStep::Ignore;
    }
    return facts.hideWhenClosed ? LifecycleStep::Hide : expire;
  case LifecycleEvent::Expired:
    if (from == Lifecycle::HiddenExpired || facts.windowOpen) {
      return LifecycleStep::Ignore;
    }
    return expire;
  case LifecycleEvent::LeaseReleased:
    return from == Lifecycle::HiddenExpired && facts.leases == 0 ? LifecycleStep::Quit : LifecycleStep::Ignore;
  case LifecycleEvent::Quit:
    return LifecycleStep::Quit;
  }
  return LifecycleStep::Ignore;
}
