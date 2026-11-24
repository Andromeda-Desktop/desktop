#include "dbus/portal/global_shortcuts_portal.h"
#include "protocols/bus_names.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "dbus/session_bus.h"

#include <set>
#include <tuple>
#include <utility>

namespace {

  constexpr Logger kLog("global-shortcuts-portal");

  const sdbus::ObjectPath kObjectPath{"/org/freedesktop/portal/desktop"};
  constexpr auto kInterface = "org.freedesktop.impl.portal.GlobalShortcuts";
  constexpr auto kRequestInterface = "org.freedesktop.impl.portal.Request";
  constexpr auto kSessionInterface = "org.freedesktop.impl.portal.Session";
  constexpr std::uint32_t kVersion = 1;
  constexpr auto kFrontendName = "org.freedesktop.portal.Desktop";
  // The compositor raises a protocol error, which ends the shell, past these limits (dsk_global_shortcuts_session_v1).
  constexpr std::size_t kMaxShortcuts = 64;
  constexpr std::size_t kMaxText = 256;

  constexpr std::uint32_t kResponseSuccess = 0;
  constexpr std::uint32_t kResponseCancelled = 1;
  constexpr std::uint32_t kResponseFailed = 2;

  using Results = std::map<std::string, sdbus::Variant>;

  std::string stringOption(const Results& properties, const std::string& key) {
    const auto it = properties.find(key);
    if (it == properties.end()) {
      return {};
    }
    try {
      return it->second.get<std::string>();
    } catch (const sdbus::Error&) {
      return {};
    }
  }

  [[noreturn]] void denyAccess() {
    throw sdbus::Error(
        sdbus::Error::Name{"org.freedesktop.DBus.Error.AccessDenied"}, "only the portal frontend may call this"
    );
  }

} // namespace

GlobalShortcutsPortal::GlobalShortcutsPortal(SessionBus& bus, GlobalShortcutsBackend& backend)
    : m_bus(bus), m_backend(backend) {
  m_dbus = sdbus::createProxy(
      m_bus.connection(), sdbus::ServiceName{busname::kDaemon}, sdbus::ObjectPath{busname::kDaemonPath}
  );
  m_frontendWatch = NameOwnerWatch::changes(
      m_bus.connection(), kFrontendName,
      [this](const std::string& /*name*/, const std::string& oldOwner, const std::string& /*newOwner*/) {
        // Every session belongs to the frontend that created it; a gained owner (D-Bus activation) has none yet.
        if (!oldOwner.empty()) {
          closeAll();
        }
      }
  );

  auto createSession = sdbus::registerMethod("CreateSession")
                           .withInputParamNames("handle", "session_handle", "app_id", "options")
                           .withOutputParamNames("response", "results");
  createSession.inputSignature = "oosa{sv}";
  createSession.outputSignature = "ua{sv}";
  createSession.callbackHandler = [this](sdbus::MethodCall call) { this->createSession(std::move(call)); };

  auto bindShortcuts = sdbus::registerMethod("BindShortcuts")
                           .withInputParamNames("handle", "session_handle", "shortcuts", "parent_window", "options")
                           .withOutputParamNames("response", "results");
  bindShortcuts.inputSignature = "ooa(sa{sv})sa{sv}";
  bindShortcuts.outputSignature = "ua{sv}";
  bindShortcuts.callbackHandler = [this](sdbus::MethodCall call) { this->bindShortcuts(std::move(call)); };

  auto listShortcuts = sdbus::registerMethod("ListShortcuts")
                           .withInputParamNames("handle", "session_handle")
                           .withOutputParamNames("response", "results");
  listShortcuts.inputSignature = "oo";
  listShortcuts.outputSignature = "ua{sv}";
  listShortcuts.callbackHandler = [this](sdbus::MethodCall call) {
    if (!fromFrontend(call)) {
      denyAccess();
    }
    sdbus::ObjectPath requestHandle;
    sdbus::ObjectPath sessionHandle;
    call >> requestHandle >> sessionHandle;
    auto reply = call.createReply();
    const auto it = m_sessions.find(sessionHandle);
    if (it == m_sessions.end()) {
      reply << kResponseFailed << Results{};
    } else {
      reply << kResponseSuccess << Results{{"shortcuts", sdbus::Variant{describe(it->second)}}};
    }
    reply.send();
  };

  m_object = sdbus::createObject(m_bus.connection(), kObjectPath);
  m_object
      ->addVTable(
          std::move(createSession), std::move(bindShortcuts), std::move(listShortcuts),
          sdbus::registerSignal("Activated")
              .withParameters<sdbus::ObjectPath, std::string, std::uint64_t, Results>(
                  "session_handle", "shortcut_id", "timestamp", "options"
              ),
          sdbus::registerSignal("Deactivated")
              .withParameters<sdbus::ObjectPath, std::string, std::uint64_t, Results>(
                  "session_handle", "shortcut_id", "timestamp", "options"
              ),
          sdbus::registerSignal("ShortcutsChanged")
              .withParameters<sdbus::ObjectPath, Shortcuts>("session_handle", "shortcuts"),
          sdbus::registerProperty("version").withGetter([]() { return kVersion; })
      )
      .forInterface(kInterface);
  kLog.info("serving {}", kInterface);
}

GlobalShortcutsPortal::~GlobalShortcutsPortal() = default;

bool GlobalShortcutsPortal::fromFrontend(const sdbus::MethodCall& call) {
  const char* sender = call.getSender();
  if (sender == nullptr) {
    return false;
  }
  try {
    std::string owner;
    m_dbus->callMethod("GetNameOwner")
        .onInterface(busname::kDaemon)
        .withArguments(std::string{kFrontendName})
        .storeResultsTo(owner);
    return owner == sender;
  } catch (const sdbus::Error& e) {
    kLog.debug("GetNameOwner({}) failed: {}", kFrontendName, e.what());
    return false;
  }
}

void GlobalShortcutsPortal::createSession(sdbus::MethodCall call) {
  if (!fromFrontend(call)) {
    denyAccess();
  }
  sdbus::ObjectPath requestHandle;
  sdbus::ObjectPath sessionHandle;
  std::string appId;
  Results options;
  call >> requestHandle >> sessionHandle >> appId >> options;
  auto reply = call.createReply();
  const std::string path = sessionHandle;
  auto backend = m_sessions.contains(path) ? nullptr : m_backend.open(appId, eventsFor(path));
  if (backend == nullptr) {
    kLog.warn("{}: no session (duplicate handle or no compositor support)", appId);
    reply << kResponseFailed << Results{};
    reply.send();
    return;
  }
  Session session;
  session.appId = appId;
  session.backend = std::move(backend);
  session.object = sdbus::createObject(m_bus.connection(), sessionHandle);
  session.object
      ->addVTable(
          sdbus::registerMethod("Close").implementedAs([this, path]() {
            handle(path, GlobalShortcutsEvent::SessionClosed);
          }),
          sdbus::registerSignal("Closed"), sdbus::registerProperty("version").withGetter([]() { return 1U; })
      )
      .forInterface(kSessionInterface);
  m_sessions.emplace(path, std::move(session));
  reply << kResponseSuccess << Results{};
  reply.send();
}

void GlobalShortcutsPortal::bindShortcuts(sdbus::MethodCall call) {
  if (!fromFrontend(call)) {
    denyAccess();
  }
  sdbus::ObjectPath requestHandle;
  sdbus::ObjectPath sessionHandle;
  Shortcuts requested;
  std::string parentWindow;
  Results options;
  call >> requestHandle >> sessionHandle >> requested >> parentWindow >> options;

  const auto fail = [&call]() {
    auto reply = call.createReply();
    reply << kResponseFailed << Results{};
    reply.send();
  };
  const auto it = m_sessions.find(sessionHandle);
  if (it == m_sessions.end()) {
    fail();
    return;
  }
  Session& session = it->second;

  std::vector<GlobalShortcut> shortcuts;
  std::set<std::string> ids;
  for (const auto& [id, properties] : requested) {
    std::string description = stringOption(properties, "description");
    std::string trigger = stringOption(properties, "preferred_trigger");
    if (id.empty() || id.size() > kMaxText || description.size() > kMaxText || !ids.insert(id).second) {
      kLog.warn("{}: refused shortcut '{}' (empty, duplicate or too long)", session.appId, id);
      fail();
      return;
    }
    if (trigger.size() > kMaxText) {
      trigger.clear();
    }
    shortcuts.push_back({.id = id, .description = std::move(description), .trigger = std::move(trigger)});
  }
  if (shortcuts.size() > kMaxShortcuts) {
    kLog.warn("{}: refused {} shortcuts (at most {})", session.appId, shortcuts.size(), kMaxShortcuts);
    fail();
    return;
  }

  const GlobalShortcutsActions actions = session.state.handle(GlobalShortcutsEvent::BindCalled);
  if (!actions.sendBind) {
    fail();
    return;
  }
  const std::string path = sessionHandle;
  const std::string requestPath = requestHandle;
  session.pendingBind = std::move(call);
  session.request = sdbus::createObject(m_bus.connection(), requestHandle);
  session.request
      ->addVTable(sdbus::registerMethod("Close").implementedAs([this, path, requestPath]() {
        const auto found = m_sessions.find(path);
        if (found != m_sessions.end()
            && found->second.request != nullptr
            && found->second.request->getObjectPath() == requestPath) {
          handle(path, GlobalShortcutsEvent::RequestClosed);
        }
      }))
      .forInterface(kRequestInterface);
  kLog.info("{} binds {} shortcuts", session.appId, shortcuts.size());
  session.backend->bind(shortcuts);
}

GlobalShortcutsEvents GlobalShortcutsPortal::eventsFor(const std::string& path) {
  return {
      .done =
          [this, path](std::vector<GlobalShortcut> shortcuts) {
            const auto it = m_sessions.find(path);
            if (it != m_sessions.end() && it->second.state.phase() != GlobalShortcutsPhase::Open) {
              it->second.shortcuts = std::move(shortcuts);
            }
            handle(path, GlobalShortcutsEvent::Done);
          },
      .cancelled = [this, path]() { handle(path, GlobalShortcutsEvent::Cancelled); },
      .revoked = [this, path]() { handle(path, GlobalShortcutsEvent::Revoked); },
      .activated = [this, path](
                       const std::string& id, std::uint32_t timeMs
                   ) { handle(path, GlobalShortcutsEvent::Activated, id, timeMs); },
      .deactivated = [this, path](
                         const std::string& id, std::uint32_t timeMs
                     ) { handle(path, GlobalShortcutsEvent::Deactivated, id, timeMs); },
  };
}

void GlobalShortcutsPortal::handle(
    const std::string& path, GlobalShortcutsEvent event, const std::string& id, std::uint32_t timeMs
) {
  const auto it = m_sessions.find(path);
  if (it == m_sessions.end()) {
    return;
  }
  Session& session = it->second;
  const GlobalShortcutsActions actions = session.state.handle(event, id, timeMs);
  if (session.state.phase() == GlobalShortcutsPhase::Open) {
    session.shortcuts.clear();
  }
  for (const auto& [heldId, pressTimeMs] : actions.release) {
    emitPress("Deactivated", path, heldId, pressTimeMs);
  }
  if (actions.emitActivated) {
    emitPress("Activated", path, id, timeMs);
  }
  if (actions.emitDeactivated) {
    emitPress("Deactivated", path, id, timeMs);
  }
  if (actions.reopen) {
    session.backend.reset();
    session.backend = m_backend.open(session.appId, eventsFor(path));
  }
  reply(session, actions.reply);
  if (actions.emitShortcutsChanged) {
    m_object->emitSignal("ShortcutsChanged")
        .onInterface(kInterface)
        .withArguments(sdbus::ObjectPath{path}, describe(session));
  }
  if (session.state.phase() == GlobalShortcutsPhase::Closed) {
    // A Close call runs inside the session object; destroy it once the call has returned.
    DeferredCall::callLater([this, path]() { m_sessions.erase(path); });
  }
}

void GlobalShortcutsPortal::reply(Session& session, GlobalShortcutsReply reply) {
  if (reply == GlobalShortcutsReply::None || !session.pendingBind.has_value()) {
    return;
  }
  auto message = session.pendingBind->createReply();
  switch (reply) {
  case GlobalShortcutsReply::Success:
    message << kResponseSuccess << Results{{"shortcuts", sdbus::Variant{describe(session)}}};
    break;
  case GlobalShortcutsReply::Cancelled:
    message << kResponseCancelled << Results{};
    break;
  case GlobalShortcutsReply::None:
  case GlobalShortcutsReply::Failed:
    message << kResponseFailed << Results{};
    break;
  }
  message.send();
  session.pendingBind.reset();
  // A Request Close call may be running inside this object.
  DeferredCall::callLater([request = std::shared_ptr<sdbus::IObject>(std::move(session.request))]() {});
}

void GlobalShortcutsPortal::emitPress(
    const char* signal, const std::string& path, const std::string& id, std::uint32_t timeMs
) {
  m_object->emitSignal(signal)
      .onInterface(kInterface)
      .withArguments(sdbus::ObjectPath{path}, id, std::uint64_t{timeMs}, Results{});
}

void GlobalShortcutsPortal::closeAll() {
  std::vector<std::string> paths;
  for (const auto& [path, session] : m_sessions) {
    paths.push_back(path);
  }
  for (const auto& path : paths) {
    handle(path, GlobalShortcutsEvent::SessionClosed);
  }
}

GlobalShortcutsPortal::Shortcuts GlobalShortcutsPortal::describe(const Session& session) {
  Shortcuts shortcuts;
  for (const GlobalShortcut& shortcut : session.shortcuts) {
    shortcuts.emplace_back(
        shortcut.id,
        Results{
            {"description", sdbus::Variant{shortcut.description}},
            {"trigger_description", sdbus::Variant{shortcut.trigger}},
        }
    );
  }
  return shortcuts;
}

std::string GlobalShortcutsPortal::list() const {
  std::string out;
  for (const auto& [path, session] : m_sessions) {
    for (const GlobalShortcut& shortcut : session.shortcuts) {
      out += session.appId + " " + shortcut.id + " " + shortcut.description + "\n";
    }
  }
  return out;
}
