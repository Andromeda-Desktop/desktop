#include "dbus/portal/global_shortcuts_session_state.h"

GlobalShortcutsActions
GlobalShortcutsSessionState::handle(GlobalShortcutsEvent event, const std::string& id, std::uint32_t timeMs) {
  using enum GlobalShortcutsPhase;
  GlobalShortcutsActions actions;
  if (m_phase == Closed) {
    if (event == GlobalShortcutsEvent::BindCalled) {
      actions.reply = GlobalShortcutsReply::Failed;
    }
    return actions;
  }
  switch (event) {
  case GlobalShortcutsEvent::BindCalled:
    if (m_phase == Approving) {
      actions.reply = GlobalShortcutsReply::Failed;
    } else {
      m_phase = Approving;
      actions.sendBind = true;
    }
    break;
  case GlobalShortcutsEvent::Done:
    if (m_phase == Approving) {
      m_phase = Bound;
      actions.reply = GlobalShortcutsReply::Success;
    } else if (m_phase == Bound) {
      actions.emitShortcutsChanged = true;
    }
    break;
  case GlobalShortcutsEvent::Cancelled:
    if (m_phase == Approving) {
      m_phase = Open;
      actions.reply = GlobalShortcutsReply::Cancelled;
    }
    break;
  case GlobalShortcutsEvent::Revoked:
    if (m_phase == Bound) {
      m_phase = Open;
      actions.emitShortcutsChanged = true;
    }
    break;
  case GlobalShortcutsEvent::Activated:
    if (m_phase == Bound && !m_held.contains(id)) {
      m_held.emplace(id, timeMs);
      actions.emitActivated = true;
    }
    break;
  case GlobalShortcutsEvent::Deactivated:
    actions.emitDeactivated = m_held.erase(id) != 0;
    break;
  case GlobalShortcutsEvent::RequestClosed:
    if (m_phase == Approving) {
      m_phase = Open;
      actions.reply = GlobalShortcutsReply::Cancelled;
      actions.reopen = true;
      actions.release.assign(m_held.begin(), m_held.end());
      m_held.clear();
    }
    break;
  case GlobalShortcutsEvent::SessionClosed:
    if (m_phase == Approving) {
      actions.reply = GlobalShortcutsReply::Cancelled;
    }
    m_phase = Closed;
    m_held.clear();
    break;
  }
  return actions;
}
