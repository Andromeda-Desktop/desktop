#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct GlobalShortcut {
  std::string id;
  std::string description;
  // XDG shortcuts syntax, for example "CTRL+ALT+m"; empty when none.
  std::string trigger;
  // dsk_global_shortcuts_session_v1 conflict; 0 means live.
  std::uint32_t conflict = 0;
};

struct GlobalShortcutsEvents {
  // The complete shortcut list after a successful bind or a change to a bound session.
  std::function<void(std::vector<GlobalShortcut>)> done;
  std::function<void()> cancelled;
  std::function<void()> revoked;
  std::function<void(const std::string& id, std::uint32_t timeMs)> activated;
  std::function<void(const std::string& id, std::uint32_t timeMs)> deactivated;
};

// One app's shortcuts in the compositor. Destruction removes its bindings and cancels a pending approval silently.
class GlobalShortcutsSession {
public:
  virtual ~GlobalShortcutsSession() = default;
  // Replaces the shortcuts; trigger is the app's preferred trigger. Never call it while a bind is pending.
  virtual void bind(const std::vector<GlobalShortcut>& shortcuts) = 0;
};

class GlobalShortcutsBackend {
public:
  virtual ~GlobalShortcutsBackend() = default;
  // nullptr when the compositor has no global shortcuts support. Events never run inside bind().
  virtual std::unique_ptr<GlobalShortcutsSession> open(const std::string& appId, GlobalShortcutsEvents events) = 0;
};
