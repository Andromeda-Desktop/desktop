#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

// One portal GlobalShortcuts session, without D-Bus or Wayland: the portal feeds it frontend calls and compositor
// events and carries out the returned actions.
enum class GlobalShortcutsPhase : std::uint8_t { Open, Approving, Bound, Closed };

enum class GlobalShortcutsEvent : std::uint8_t {
  BindCalled,
  Done,
  Cancelled,
  Revoked,
  Activated,
  Deactivated,
  RequestClosed,
  SessionClosed,
};

enum class GlobalShortcutsReply : std::uint8_t { None, Success, Cancelled, Failed };

struct GlobalShortcutsActions {
  GlobalShortcutsReply reply = GlobalShortcutsReply::None;
  bool sendBind = false;
  // Destroy the compositor session (this cancels its pending approval) and open a new one.
  bool reopen = false;
  bool emitShortcutsChanged = false;
  bool emitActivated = false;
  bool emitDeactivated = false;
  // Held shortcuts the compositor will never release; emit Deactivated for each with its press time.
  std::vector<std::pair<std::string, std::uint32_t>> release;
};

class GlobalShortcutsSessionState {
public:
  // id and timeMs apply to Activated and Deactivated only.
  GlobalShortcutsActions handle(GlobalShortcutsEvent event, const std::string& id = {}, std::uint32_t timeMs = 0);

  [[nodiscard]] GlobalShortcutsPhase phase() const noexcept { return m_phase; }
  [[nodiscard]] bool held(const std::string& id) const { return m_held.contains(id); }

private:
  GlobalShortcutsPhase m_phase = GlobalShortcutsPhase::Open;
  std::map<std::string, std::uint32_t> m_held;
};
