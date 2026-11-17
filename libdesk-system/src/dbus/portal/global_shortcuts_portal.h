#pragma once

#include "dbus/portal/global_shortcuts_backend.h"
#include "dbus/portal/global_shortcuts_session_state.h"

#include <map>
#include <memory>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <vector>

class SessionBus;

// Serves org.freedesktop.impl.portal.GlobalShortcuts under org.freedesktop.impl.portal.desktop.andromeda.shell.
// Each portal session is one compositor session: the compositor assigns triggers, asks the user to approve them, and
// sends presses. Only the owner of org.freedesktop.portal.Desktop may call it, because approvals are keyed by app id.
class GlobalShortcutsPortal {
public:
  GlobalShortcutsPortal(SessionBus& bus, GlobalShortcutsBackend& backend);
  ~GlobalShortcutsPortal();

  GlobalShortcutsPortal(const GlobalShortcutsPortal&) = delete;
  GlobalShortcutsPortal& operator=(const GlobalShortcutsPortal&) = delete;

  // One "app-id shortcut-id description" line per shortcut of a bound session.
  [[nodiscard]] std::string list() const;

private:
  using Results = std::map<std::string, sdbus::Variant>;
  using Shortcuts = std::vector<sdbus::Struct<std::string, Results>>;

  struct Session {
    std::string appId;
    std::unique_ptr<sdbus::IObject> object;
    std::unique_ptr<GlobalShortcutsSession> backend;
    GlobalShortcutsSessionState state;
    std::vector<GlobalShortcut> shortcuts;
    std::optional<sdbus::MethodCall> pendingBind;
    std::unique_ptr<sdbus::IObject> request;
  };

  void createSession(sdbus::MethodCall call);
  void bindShortcuts(sdbus::MethodCall call);
  [[nodiscard]] bool fromFrontend(const sdbus::MethodCall& call);
  [[nodiscard]] GlobalShortcutsEvents eventsFor(const std::string& path);
  void
  handle(const std::string& path, GlobalShortcutsEvent event, const std::string& id = {}, std::uint32_t timeMs = 0);
  void reply(Session& session, GlobalShortcutsReply reply);
  void emitPress(const char* signal, const std::string& path, const std::string& id, std::uint32_t timeMs);
  void closeAll();
  [[nodiscard]] static Shortcuts describe(const Session& session);

  SessionBus& m_bus;
  GlobalShortcutsBackend& m_backend;
  std::unique_ptr<sdbus::IObject> m_object;
  std::unique_ptr<sdbus::IProxy> m_dbus;
  sdbus::Slot m_frontendWatch;
  std::map<std::string, Session> m_sessions;
};
