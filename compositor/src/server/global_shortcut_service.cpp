#include "server/global_shortcut_service.h"

#include "config/config.h"
#include "core/log.h"
#include "desktop-unstable-v1-protocol.h"
#include "input/event_time.h"
#include "input/input_capture.h"
#include "scene/quit_confirm.h"
#include "server/server.h"
#include "wlr.h"

#include <algorithm>
#include <format>
#include <utility>

namespace andromeda {

  namespace {
    constexpr Logger kLog("global-shortcuts");
    constexpr size_t kMaxShortcuts = 64;
    constexpr size_t kMaxText = 256;
    constexpr size_t kRowsPerPage = 8;

    static_assert(shortcut_mod::kShift == WLR_MODIFIER_SHIFT && shortcut_mod::kCtrl == WLR_MODIFIER_CTRL
                  && shortcut_mod::kAlt == WLR_MODIFIER_ALT && shortcut_mod::kLogo == WLR_MODIFIER_LOGO);
    static_assert(static_cast<uint32_t>(ShortcutConflict::None) == DSK_GLOBAL_SHORTCUTS_SESSION_V1_CONFLICT_NONE
                  && static_cast<uint32_t>(ShortcutConflict::Unassigned)
                         == DSK_GLOBAL_SHORTCUTS_SESSION_V1_CONFLICT_UNASSIGNED
                  && static_cast<uint32_t>(ShortcutConflict::Disabled) == DSK_GLOBAL_SHORTCUTS_SESSION_V1_CONFLICT_DISABLED
                  && static_cast<uint32_t>(ShortcutConflict::Reserved) == DSK_GLOBAL_SHORTCUTS_SESSION_V1_CONFLICT_RESERVED
                  && static_cast<uint32_t>(ShortcutConflict::InUse) == DSK_GLOBAL_SHORTCUTS_SESSION_V1_CONFLICT_IN_USE);

    struct ResourceData {
      GlobalShortcutService* service = nullptr;
      ShortcutSessionId session = 0;
    };

    ResourceData* dataFrom(wl_resource* resource) { return static_cast<ResourceData*>(wl_resource_get_user_data(resource)); }

    // Printable ASCII only: the label comes from the requester.
    std::string label(std::string_view text) {
      std::string out;
      for (const unsigned char character : text) {
        out += character >= 32 && character < 127 ? static_cast<char>(character) : '?';
      }
      return out;
    }
  } // namespace

  GlobalShortcutService::GlobalShortcutService(Server& server)
      : m_server(server), m_registry([this](const ShortcutChord& chord) { return reserved(chord); }) {
    if (auto grants = m_store.load()) {
      m_registry.setGrants(std::move(*grants));
    } else {
      kLog.warn(
          "{} is unsafe or malformed; approvals this session are not saved and the file is left as is",
          m_store.path().string()
      );
    }
    applyConfig();
  }

  GlobalShortcutService::~GlobalShortcutService() {
    if (m_refreshIdle != nullptr) {
      wl_event_source_remove(m_refreshIdle);
    }
    for (auto& [id, session] : m_sessions) {
      delete dataFrom(session.resource);
      wl_resource_set_user_data(session.resource, nullptr);
    }
  }

  void GlobalShortcutService::createSession(wl_client* client, uint32_t id, std::string appId) {
    wl_resource* resource = wl_resource_create(client, &dsk_global_shortcuts_session_v1_interface, 1, id);
    if (resource == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    static const struct dsk_global_shortcuts_session_v1_interface implementation = {
        .destroy = handleDestroy,
        .add_shortcut = handleAddShortcut,
        .bind = handleBind,
    };
    const ShortcutSessionId session = m_registry.open(std::move(appId));
    wl_resource_set_implementation(resource, &implementation, new ResourceData{this, session}, handleResourceDestroyed);
    m_sessions[session].resource = resource;
  }

  void GlobalShortcutService::handleDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

  void GlobalShortcutService::handleAddShortcut(
      wl_client*, wl_resource* resource, const char* id, const char* description, const char* preferred
  ) {
    ResourceData* data = dataFrom(resource);
    if (data == nullptr) {
      return;
    }
    auto found = data->service->m_sessions.find(data->session);
    if (found == data->service->m_sessions.end()) {
      return;
    }
    auto& staged = found->second.staged;
    const std::string_view idText(id);
    const std::string_view descriptionText(description);
    if (idText.empty() || idText.size() > kMaxText || descriptionText.size() > kMaxText || staged.size() >= kMaxShortcuts
        || std::ranges::any_of(staged, [&](const ShortcutRequest& request) { return request.id == idText; })) {
      wl_resource_post_error(
          resource, DSK_GLOBAL_SHORTCUTS_SESSION_V1_ERROR_INVALID_SHORTCUT, "invalid or duplicate shortcut '%s'", id
      );
      return;
    }
    staged.push_back({
        .id = std::string(idText),
        .description = std::string(descriptionText),
        .preferred = *preferred != '\0' ? parsePortalTrigger(preferred) : std::nullopt,
    });
  }

  void GlobalShortcutService::handleBind(wl_client*, wl_resource* resource) {
    ResourceData* data = dataFrom(resource);
    if (data == nullptr) {
      return;
    }
    GlobalShortcutService& self = *data->service;
    Session& session = self.m_sessions.at(data->session);
    if (self.m_registry.phase(data->session) == ShortcutRegistry::Phase::Approving) {
      wl_resource_post_error(
          resource, DSK_GLOBAL_SHORTCUTS_SESSION_V1_ERROR_BIND_IN_PROGRESS, "bind while an approval is pending"
      );
      return;
    }
    session.requested = std::exchange(session.staged, {});
    const auto result = self.m_registry.bind(data->session, session.requested);
    if (result == ShortcutRegistry::BindResult::Bound) {
      // A re-bind with the same states still answers, so the shell's request completes.
      session.sentStates.clear();
      session.sentPhase = ShortcutRegistry::Phase::Unbound;
    }
    self.sync(monotonicMsec());
    self.refreshApproval();
  }

  void GlobalShortcutService::handleResourceDestroyed(wl_resource* resource) {
    ResourceData* data = dataFrom(resource);
    if (data == nullptr) {
      return;
    }
    GlobalShortcutService& self = *data->service;
    const ShortcutSessionId session = data->session;
    delete data;
    self.m_sessions.erase(session);
    const bool shown = self.m_showing && self.m_showing->session == session;
    self.m_registry.close(session);
    self.sync(monotonicMsec());
    if (shown) {
      self.m_showing.reset();
      if (QuitConfirm* confirm = self.m_server.quitConfirm(); confirm != nullptr) {
        confirm->hide();
      }
    }
  }

  bool GlobalShortcutService::reserved(const ShortcutChord& chord) const {
    const uint32_t mod = m_server.modKey();
    return std::ranges::any_of(config().keybinds, [&](const Keybind& bind) {
      return !bind.modifierOnly && bind.wheel == WheelDirection::None && bind.mouseButton == 0
             && bind.keysym == chord.keysym && (bind.modifiers | (bind.useMod ? mod : 0U)) == chord.modifiers;
    });
  }

  bool GlobalShortcutService::allowed() const {
    const QuitConfirm* confirm = m_server.quitConfirm();
    const InputCapture* capture = m_server.inputCapture();
    return !m_server.sessionLocked() && m_server.sessionActive() && !m_server.keyboardShortcutsInhibited()
           && (confirm == nullptr || !confirm->visible()) && (capture == nullptr || !capture->active());
  }

  bool GlobalShortcutService::press(
      ShortcutKeyTracker::Keyboard keyboard, uint32_t keycode, uint32_t keysym, uint32_t rawKeysym, uint32_t modifiers,
      uint32_t timeMsec
  ) {
    const uint32_t effective = modifiers & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
    std::vector<ShortcutEvent> events;
    const bool ok = allowed();
    bool consumed = m_keys.press(
        keyboard, keycode, {.modifiers = effective, .keysym = xkb_keysym_to_lower(keysym)}, ok, m_registry, timeMsec,
        events
    );
    if (!consumed && rawKeysym != XKB_KEY_NoSymbol) {
      consumed = m_keys.press(
          keyboard, keycode, {.modifiers = effective, .keysym = rawKeysym}, ok, m_registry, timeMsec, events
      );
    }
    send(events);
    return consumed;
  }

  bool GlobalShortcutService::release(ShortcutKeyTracker::Keyboard keyboard, uint32_t keycode, uint32_t timeMsec) {
    std::vector<ShortcutEvent> events;
    const bool consumed = m_keys.release(keyboard, keycode, timeMsec, events);
    send(events);
    return consumed;
  }

  void GlobalShortcutService::suspend() {
    std::vector<ShortcutEvent> events;
    m_keys.suspend(monotonicMsec(), events);
    send(events);
  }

  void GlobalShortcutService::removeKeyboard(ShortcutKeyTracker::Keyboard keyboard) {
    std::vector<ShortcutEvent> events;
    m_keys.removeKeyboard(keyboard, monotonicMsec(), events);
    send(events);
  }

  void GlobalShortcutService::applyConfig() {
    const uint32_t mod = m_server.modKey();
    ShortcutOverrides overrides;
    for (const auto& [app, shortcuts] : config().globalShortcuts) {
      auto& target = overrides[app];
      for (const auto& [id, chord] : shortcuts) {
        target[id] = chord ? std::optional<ShortcutChord>(ShortcutChord{
                                 .modifiers = chord->modifiers | (chord->useMod ? mod : 0U), .keysym = chord->keysym
                             })
                           : std::nullopt;
      }
    }
    m_registry.setOverrides(std::move(overrides));
    // Keybinds may have changed what is reserved.
    m_registry.setReserved([this](const ShortcutChord& chord) { return reserved(chord); });
    sync(monotonicMsec());
  }

  void GlobalShortcutService::revoke(std::string_view app) {
    m_registry.revoke(app);
    if (const auto merged = m_store.update(app, {})) {
      m_registry.setGrants(*merged);
    } else {
      kLog.warn("could not save the revoke of '{}' to {}", app, m_store.path().string());
    }
    sync(monotonicMsec());
  }

  void GlobalShortcutService::persist(const std::string& app) {
    const auto& grants = m_registry.grants();
    const auto found = grants.find(app);
    const auto merged =
        m_store.update(app, found == grants.end() ? std::map<std::string, ShortcutChord>{} : found->second);
    if (!merged) {
      kLog.warn("could not save the approval of '{}' to {}", app, m_store.path().string());
      return;
    }
    m_registry.setGrants(*merged);
  }

  void GlobalShortcutService::sync(uint32_t timeMsec) {
    std::vector<ShortcutEvent> events;
    m_keys.reconcile(m_registry, timeMsec, events);
    send(events);
    for (auto& [id, session] : m_sessions) {
      const auto phase = m_registry.phase(id);
      const auto states = m_registry.shortcuts(id);
      const bool wasBound = session.sentPhase == ShortcutRegistry::Phase::Bound;
      if (phase == ShortcutRegistry::Phase::Bound) {
        const bool changed = !wasBound || states.size() != session.sentStates.size()
                             || !std::ranges::equal(states, session.sentStates, [](const auto& a, const auto& b) {
                                  return a.id == b.id && a.description == b.description && a.chord == b.chord
                                         && a.conflict == b.conflict;
                                });
        if (changed) {
          for (const ShortcutState& state : states) {
            dsk_global_shortcuts_session_v1_send_shortcut(
                session.resource, state.id.c_str(), state.description.c_str(),
                state.chord ? describeChord(*state.chord).c_str() : "", static_cast<uint32_t>(state.conflict)
            );
          }
          dsk_global_shortcuts_session_v1_send_done(session.resource);
        }
      } else if (wasBound && phase == ShortcutRegistry::Phase::Unbound) {
        dsk_global_shortcuts_session_v1_send_revoked(session.resource);
      } else if (session.sentPhase == ShortcutRegistry::Phase::Approving && phase == ShortcutRegistry::Phase::Unbound) {
        dsk_global_shortcuts_session_v1_send_cancelled(session.resource);
      }
      session.sentPhase = phase;
      session.sentStates = phase == ShortcutRegistry::Phase::Bound ? states : std::vector<ShortcutState>{};
    }
  }

  void GlobalShortcutService::send(const std::vector<ShortcutEvent>& events) {
    for (const ShortcutEvent& event : events) {
      const auto found = m_sessions.find(event.session);
      if (found == m_sessions.end()) {
        continue;
      }
      if (event.kind == ShortcutEvent::Kind::Activated) {
        dsk_global_shortcuts_session_v1_send_activated(found->second.resource, event.id.c_str(), event.timeMsec);
      } else {
        dsk_global_shortcuts_session_v1_send_deactivated(found->second.resource, event.id.c_str(), event.timeMsec);
      }
    }
  }

  std::vector<std::string> GlobalShortcutService::approvalRows(ShortcutSessionId session) const {
    std::vector<std::string> rows;
    const auto found = m_sessions.find(session);
    if (found == m_sessions.end()) {
      return rows;
    }
    for (const ShortcutRequest& request : found->second.requested) {
      const std::string name = label(request.description.empty() ? request.id : request.description);
      const bool usable = request.preferred && preferredChordAllowed(*request.preferred);
      rows.push_back(std::format("{}  {}", name, usable ? describeChord(*request.preferred) : "(no key yet)"));
    }
    return rows;
  }

  void GlobalShortcutService::refreshApproval() {
    QuitConfirm* confirm = m_server.quitConfirm();
    const auto head = m_registry.pendingApproval();
    if (confirm == nullptr || !head || m_showing || confirm->visible() || m_server.sessionLocked()
        || !m_server.sessionActive()) {
      return;
    }
    m_showing = Showing{.session = *head, .page = 0};
    showApproval();
  }

  void GlobalShortcutService::scheduleRefresh() {
    if (m_refreshIdle != nullptr) {
      return;
    }
    m_refreshIdle = wl_event_loop_add_idle(
        wl_display_get_event_loop(m_server.display()),
        [](void* data) {
          auto* self = static_cast<GlobalShortcutService*>(data);
          self->m_refreshIdle = nullptr;
          self->refreshApproval();
        },
        this
    );
  }

  void GlobalShortcutService::showApproval() {
    const std::vector<std::string> rows = approvalRows(m_showing->session);
    const size_t pages = std::max<size_t>(1, (rows.size() + kRowsPerPage - 1) / kRowsPerPage);
    const size_t first = m_showing->page * kRowsPerPage;
    const std::vector<std::string> page(
        rows.begin() + static_cast<ptrdiff_t>(std::min(first, rows.size())),
        rows.begin() + static_cast<ptrdiff_t>(std::min(first + kRowsPerPage, rows.size()))
    );
    const std::string* app = m_registry.app(m_showing->session);
    suspend();
    QuitConfirm* confirm = m_server.quitConfirm();
    confirm->showGlobalShortcuts(
        label(app != nullptr && !app->empty() ? *app : "Unknown application"), page, m_showing->page, pages
    );
    if (!confirm->visible()) {
      denyApproval();
    }
  }

  void GlobalShortcutService::approvalEnter() {
    if (!m_showing) {
      return;
    }
    const size_t rows = approvalRows(m_showing->session).size();
    if ((m_showing->page + 1) * kRowsPerPage < rows) {
      ++m_showing->page;
      showApproval();
      return;
    }
    const ShortcutSessionId session = std::exchange(m_showing, std::nullopt)->session;
    const std::string* app = m_registry.app(session);
    const std::string appId = app != nullptr ? *app : std::string{};
    if (m_registry.approve(session)) {
      persist(appId);
    }
    sync(monotonicMsec());
    m_server.quitConfirm()->hide();
  }

  void GlobalShortcutService::denyApproval() {
    if (!m_showing) {
      return;
    }
    m_registry.cancel(std::exchange(m_showing, std::nullopt)->session);
    sync(monotonicMsec());
  }

} // namespace andromeda
