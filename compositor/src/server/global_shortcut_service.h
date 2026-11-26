#pragma once

#include "input/global_shortcut_store.h"
#include "input/global_shortcuts.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct wl_client;
struct wl_event_source;
struct wl_resource;

namespace andromeda {

  class Server;

  // Serves dsk_global_shortcuts_session_v1: owns the registry, the key tracker, the grant store and the approval page.
  class GlobalShortcutService {
  public:
    explicit GlobalShortcutService(Server& server);
    ~GlobalShortcutService();

    GlobalShortcutService(const GlobalShortcutService&) = delete;
    GlobalShortcutService& operator=(const GlobalShortcutService&) = delete;

    void createSession(wl_client* client, uint32_t id, std::string appId);

    // A press no compositor keybind took. True when it belongs to a live shortcut and must not reach clients.
    bool press(
        ShortcutKeyTracker::Keyboard keyboard, uint32_t keycode, uint32_t keysym, uint32_t rawKeysym, uint32_t modifiers,
        uint32_t timeMsec
    );
    bool release(ShortcutKeyTracker::Keyboard keyboard, uint32_t keycode, uint32_t timeMsec);
    // Ends every held shortcut: lock, inhibitor, input capture, a confirmation page, session inactive.
    void suspend();
    void removeKeyboard(ShortcutKeyTracker::Keyboard keyboard);
    // [global_shortcuts], keybinds or the Mod key changed.
    void applyConfig();
    // Forgets the app's grants (all apps when empty) and unbinds its sessions at once; `done` gets false when the
    // store write fails.
    void revoke(std::string_view app, std::function<void(bool)> done);
    struct Assigned {
      enum class Result : uint8_t { Ok, BadChord, Reserved, InUse, WriteFailed } result;
      std::string holder;  // the app that holds the chord, for InUse
    };
    // Sets one shortcut's chord (keybind syntax, "" turns it off) in the settings file and reloads; `done` gets the
    // result once the file is written. Writes nothing when the chord is bad, a compositor keybind, or another app's
    // shortcut.
    void assign(
        std::string_view app, std::string_view id, std::string_view chord, std::function<void(Assigned)> done
    );
    [[nodiscard]] const ShortcutRegistry& registry() const { return m_registry; }

    // The approval page: Enter on the last page approves, Enter on another page turns it, anything else denies.
    void approvalEnter();
    void denyApproval();
    // Shows the head of the approval queue when the confirmation panel is free and the session allows it.
    void refreshApproval();
    // refreshApproval on the next idle, once the change that freed the panel has settled.
    void scheduleRefresh();

  private:
    struct Session {
      wl_resource* resource = nullptr;
      std::vector<ShortcutRequest> staged;
      // The last bind's shortcuts, for the approval page.
      std::vector<ShortcutRequest> requested;
      ShortcutRegistry::Phase sentPhase = ShortcutRegistry::Phase::Unbound;
      std::vector<ShortcutState> sentStates;
    };
    struct Showing {
      ShortcutSessionId session = 0;
      size_t page = 0;
    };

    static void handleDestroy(wl_client* client, wl_resource* resource);
    static void handleAddShortcut(
        wl_client* client, wl_resource* resource, const char* id, const char* description, const char* preferred
    );
    static void handleBind(wl_client* client, wl_resource* resource);
    static void handleResourceDestroyed(wl_resource* resource);

    [[nodiscard]] bool reserved(const ShortcutChord& chord) const;
    [[nodiscard]] bool allowed() const;
    [[nodiscard]] std::vector<std::string> approvalRows(ShortcutSessionId session) const;
    void showApproval();
    void persist(const std::string& app);
    // After every registry change: ends held shortcuts that went away and tells each session its new state.
    void sync(uint32_t timeMsec);
    void send(const std::vector<ShortcutEvent>& events);

    Server& m_server;
    ShortcutRegistry m_registry;
    ShortcutKeyTracker m_keys;
    ShortcutGrantSync m_grants;
    std::map<ShortcutSessionId, Session> m_sessions;
    std::optional<Showing> m_showing;
    wl_event_source* m_refreshIdle = nullptr;
  };

} // namespace andromeda
