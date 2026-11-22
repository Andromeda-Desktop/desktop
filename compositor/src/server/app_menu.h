#pragma once

#include "wlr.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace andromeda {

  // Where a window's menus live on the session bus, for the macOS-layout top bar (board desktop-menus.html A5).
  struct AppMenuAddress {
    enum class Kind : uint8_t { DbusMenu, Gtk };
    Kind kind = Kind::DbusMenu;
    // DbusMenu: com.canonical.dbusmenu at service + path (org_kde_kwin_appmenu, Qt/KDE apps).
    // Gtk: org.gtk.Menus at service + path (the menubar), org.gtk.Actions at the app and window paths.
    std::string service;
    std::string path;
    std::string applicationPath;
    std::string windowPath;
    bool operator==(const AppMenuAddress&) const = default;
  };

  // One item of a dsk_app_menu_v1 tree; parent 0 is the bar.
  struct DskMenuItem {
    uint32_t id = 0;
    uint32_t parent = 0;
    uint32_t flags = 0;
    std::string label;
    std::string shortcut;
  };

  // A tree a libdesk app sent over dsk_app_menu_v1. The serial changes with each commit, so an activation the shell
  // sends for an older tree is refused.
  struct DskMenu {
    uint64_t serial = 0;
    std::vector<DskMenuItem> items;
  };

  using AppMenu = std::variant<std::monostate, AppMenuAddress, DskMenu>;

  // Serves org_kde_kwin_appmenu_manager, gtk_shell1 (version 1: dbus properties only) and dsk_app_menu_manager_v1,
  // and keeps each surface's menu. A changed menu calls the change callback.
  class AppMenuRegistry {
  public:
    AppMenuRegistry(wl_display* display, std::function<void()> onChange);
    ~AppMenuRegistry();
    AppMenuRegistry(const AppMenuRegistry&) = delete;
    AppMenuRegistry& operator=(const AppMenuRegistry&) = delete;

    [[nodiscard]] const AppMenu* menuFor(const wlr_surface* surface) const;
    // Sends activated(id) to the client when serial is the surface's current dsk tree and id an enabled leaf.
    [[nodiscard]] bool activate(const wlr_surface* surface, uint64_t serial, uint32_t id) const;

    struct Entry;

  private:
    friend struct AppMenuCallbacks;
    Entry& entryFor(wlr_surface* surface);
    void destroy(Entry* entry);
    void changed();

    wl_global* m_kdeGlobal = nullptr;
    wl_global* m_gtkGlobal = nullptr;
    wl_global* m_dskGlobal = nullptr;
    uint64_t m_nextSerial = 1;
    std::function<void()> m_onChange;
    std::vector<std::unique_ptr<Entry>> m_entries;
  };

} // namespace andromeda
