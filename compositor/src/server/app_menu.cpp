#include "server/app_menu.h"

#include "dsk-app-menu-v1-protocol.h"
#include "gtk-shell-protocol.h"
#include "kde-appmenu-protocol.h"

#include <algorithm>
#include <utility>

namespace andromeda {

  namespace {
    constexpr uint32_t kKdeVersion = 1;
    // Version 1 has the dbus properties; later versions hand title-bar gestures to the compositor, which we do not do.
    constexpr uint32_t kGtkVersion = 1;
    constexpr uint32_t kDskVersion = 1;
    constexpr size_t kDskMaxItems = 1024;

    std::string str(const char* value) { return value != nullptr ? value : ""; }
  } // namespace

  struct AppMenuRegistry::Entry {
    AppMenuRegistry* registry = nullptr;
    wlr_surface* surface = nullptr;
    wlr_addon addon{};
    AppMenu menu;
    // org_kde_kwin_appmenu and gtk_surface1 objects of the surface.
    std::vector<wl_resource*> resources;
    wl_resource* dskResource = nullptr;
    std::vector<DskMenuItem> pending;
  };

  struct AppMenuCallbacks {
    static const wlr_addon_interface kAddon;
    static const struct org_kde_kwin_appmenu_manager_interface kKdeManager;
    static const struct org_kde_kwin_appmenu_interface kKdeMenu;
    static const struct gtk_shell1_interface kGtkShell;
    static const struct gtk_surface1_interface kGtkSurface;
    static const struct dsk_app_menu_manager_v1_interface kDskManager;
    static const struct dsk_app_menu_v1_interface kDskMenu;

    static AppMenuRegistry::Entry* entry(wl_resource* resource) {
      return static_cast<AppMenuRegistry::Entry*>(wl_resource_get_user_data(resource));
    }

    static void setMenu(AppMenuRegistry::Entry* state, AppMenu menu) {
      if (state == nullptr) {
        return;
      }
      const auto* oldAddress = std::get_if<AppMenuAddress>(&state->menu);
      const auto* newAddress = std::get_if<AppMenuAddress>(&menu);
      const bool same = state->menu.index() == menu.index()
          && (std::holds_alternative<std::monostate>(menu) || (oldAddress != nullptr && newAddress != nullptr && *oldAddress == *newAddress));
      if (!same) {
        state->menu = std::move(menu);
        state->registry->changed();
      }
    }

    static void setAddress(wl_resource* resource, std::optional<AppMenuAddress> address) {
      setMenu(entry(resource), address.has_value() ? AppMenu{std::move(*address)} : AppMenu{});
    }

    static void resourceDestroyed(wl_resource* resource) {
      AppMenuRegistry::Entry* state = entry(resource);
      if (state == nullptr) {
        return;
      }
      std::erase(state->resources, resource);
      if (state->resources.empty() && std::holds_alternative<AppMenuAddress>(state->menu)) {
        setMenu(state, {});
      }
    }

    static void dskResourceDestroyed(wl_resource* resource) {
      AppMenuRegistry::Entry* state = entry(resource);
      if (state == nullptr) {
        return;
      }
      state->dskResource = nullptr;
      state->pending.clear();
      if (std::holds_alternative<DskMenu>(state->menu)) {
        setMenu(state, {});
      }
    }

    static void dskItem(wl_client* /*client*/, wl_resource* resource, uint32_t id, uint32_t parent, uint32_t flags,
                        const char* label, const char* shortcut) {
      AppMenuRegistry::Entry* state = entry(resource);
      if (state == nullptr) {
        return;
      }
      if (state->pending.size() >= kDskMaxItems) {
        wl_resource_post_error(resource, DSK_APP_MENU_V1_ERROR_TOO_LARGE, "more than %zu items", kDskMaxItems);
        return;
      }
      const auto sent = [&](uint32_t wanted) {
        return std::ranges::any_of(state->pending, [&](const DskMenuItem& item) { return item.id == wanted; });
      };
      if (id == 0 || sent(id) || (parent != 0 && !sent(parent))) {
        wl_resource_post_error(resource, DSK_APP_MENU_V1_ERROR_INVALID_ITEM, "invalid item %u (parent %u)", id, parent);
        return;
      }
      state->pending.push_back(
          DskMenuItem{.id = id, .parent = parent, .flags = flags, .label = str(label), .shortcut = str(shortcut)});
    }

    static void dskCommit(wl_client* /*client*/, wl_resource* resource) {
      AppMenuRegistry::Entry* state = entry(resource);
      if (state == nullptr) {
        return;
      }
      std::vector<DskMenuItem> items = std::exchange(state->pending, {});
      if (items.empty()) {
        setMenu(state, {});
        return;
      }
      setMenu(state, DskMenu{.serial = state->registry->m_nextSerial++, .items = std::move(items)});
    }

    static void bindDsk(wl_client* client, void* data, uint32_t version, uint32_t id) {
      wl_resource* resource =
          wl_resource_create(client, &dsk_app_menu_manager_v1_interface, static_cast<int>(version), id);
      if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
      }
      wl_resource_set_implementation(resource, &kDskManager, data, nullptr);
    }

    static void attach(wl_client* client, wl_resource* parent, const wl_interface* interface, const void* impl,
                       uint32_t id, wl_resource* surfaceResource, AppMenuRegistry* self) {
      wl_resource* resource = wl_resource_create(client, interface, wl_resource_get_version(parent), id);
      if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
      }
      AppMenuRegistry::Entry& state = self->entryFor(wlr_surface_from_resource(surfaceResource));
      state.resources.push_back(resource);
      wl_resource_set_implementation(resource, impl, &state, resourceDestroyed);
    }

    static void bindKde(wl_client* client, void* data, uint32_t version, uint32_t id) {
      wl_resource* resource =
          wl_resource_create(client, &org_kde_kwin_appmenu_manager_interface, static_cast<int>(version), id);
      if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
      }
      wl_resource_set_implementation(resource, &kKdeManager, data, nullptr);
    }

    static void bindGtk(wl_client* client, void* data, uint32_t version, uint32_t id) {
      wl_resource* resource = wl_resource_create(client, &gtk_shell1_interface, static_cast<int>(version), id);
      if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
      }
      wl_resource_set_implementation(resource, &kGtkShell, data, nullptr);
      // No global menu bar capability: GTK keeps its in-window menu bars.
      gtk_shell1_send_capabilities(resource, 0);
    }
  };

  const wlr_addon_interface AppMenuCallbacks::kAddon = {
      .name = "andromeda_app_menu",
      .destroy =
          [](wlr_addon* addon) {
            AppMenuRegistry::Entry* state = wl_container_of(addon, state, addon);
            state->registry->destroy(state);
          },
  };

  const struct org_kde_kwin_appmenu_manager_interface AppMenuCallbacks::kKdeManager = {
      .create =
          [](wl_client* client, wl_resource* resource, uint32_t id, wl_resource* surface) {
            attach(client, resource, &org_kde_kwin_appmenu_interface, &kKdeMenu, id, surface,
                   static_cast<AppMenuRegistry*>(wl_resource_get_user_data(resource)));
          },
  };

  const struct org_kde_kwin_appmenu_interface AppMenuCallbacks::kKdeMenu = {
      .set_address =
          [](wl_client*, wl_resource* resource, const char* service, const char* path) {
            setAddress(resource, AppMenuAddress{.kind = AppMenuAddress::Kind::DbusMenu, .service = str(service),
                                                .path = str(path), .applicationPath = {}, .windowPath = {}});
          },
      .release = [](wl_client*, wl_resource* resource) { wl_resource_destroy(resource); },
  };

  const struct gtk_shell1_interface AppMenuCallbacks::kGtkShell = {
      .get_gtk_surface =
          [](wl_client* client, wl_resource* resource, uint32_t id, wl_resource* surface) {
            attach(client, resource, &gtk_surface1_interface, &kGtkSurface, id, surface,
                   static_cast<AppMenuRegistry*>(wl_resource_get_user_data(resource)));
          },
      .set_startup_id = [](wl_client*, wl_resource*, const char*) {},
      .system_bell = [](wl_client*, wl_resource*, wl_resource*) {},
      .notify_launch = nullptr,
  };

  const struct gtk_surface1_interface AppMenuCallbacks::kGtkSurface = {
      .set_dbus_properties =
          [](wl_client*, wl_resource* resource, const char* /*applicationId*/, const char* /*appMenuPath*/,
             const char* menubarPath, const char* windowPath, const char* applicationPath, const char* busName) {
            if (menubarPath == nullptr || busName == nullptr) {
              setAddress(resource, std::nullopt);
              return;
            }
            setAddress(resource, AppMenuAddress{.kind = AppMenuAddress::Kind::Gtk, .service = busName,
                                                .path = menubarPath, .applicationPath = str(applicationPath),
                                                .windowPath = str(windowPath)});
          },
      .set_modal = [](wl_client*, wl_resource*) {},
      .unset_modal = [](wl_client*, wl_resource*) {},
      .present = [](wl_client*, wl_resource*, uint32_t) {},
      .request_focus = nullptr,
      .release = nullptr,
      .titlebar_gesture = nullptr,
      .set_a11y_properties = nullptr,
  };

  const struct dsk_app_menu_manager_v1_interface AppMenuCallbacks::kDskManager = {
      .destroy = [](wl_client*, wl_resource* resource) { wl_resource_destroy(resource); },
      .get_menu =
          [](wl_client* client, wl_resource* manager, uint32_t id, wl_resource* toplevelResource) {
            auto* self = static_cast<AppMenuRegistry*>(wl_resource_get_user_data(manager));
            wlr_xdg_toplevel* toplevel = wlr_xdg_toplevel_from_resource(toplevelResource);
            AppMenuRegistry::Entry& state = self->entryFor(toplevel->base->surface);
            if (state.dskResource != nullptr) {
              wl_resource_post_error(manager, DSK_APP_MENU_MANAGER_V1_ERROR_ALREADY_EXISTS,
                                     "the toplevel already has a dsk_app_menu_v1");
              return;
            }
            wl_resource* resource =
                wl_resource_create(client, &dsk_app_menu_v1_interface, wl_resource_get_version(manager), id);
            if (resource == nullptr) {
              wl_client_post_no_memory(client);
              return;
            }
            state.dskResource = resource;
            wl_resource_set_implementation(resource, &kDskMenu, &state, dskResourceDestroyed);
          },
  };

  const struct dsk_app_menu_v1_interface AppMenuCallbacks::kDskMenu = {
      .destroy = [](wl_client*, wl_resource* resource) { wl_resource_destroy(resource); },
      .item = dskItem,
      .commit = dskCommit,
  };

  AppMenuRegistry::AppMenuRegistry(wl_display* display, std::function<void()> onChange)
      : m_onChange(std::move(onChange)) {
    m_kdeGlobal =
        wl_global_create(display, &org_kde_kwin_appmenu_manager_interface, kKdeVersion, this, AppMenuCallbacks::bindKde);
    m_gtkGlobal = wl_global_create(display, &gtk_shell1_interface, kGtkVersion, this, AppMenuCallbacks::bindGtk);
    m_dskGlobal =
        wl_global_create(display, &dsk_app_menu_manager_v1_interface, kDskVersion, this, AppMenuCallbacks::bindDsk);
  }

  AppMenuRegistry::~AppMenuRegistry() {
    while (!m_entries.empty()) {
      destroy(m_entries.back().get());
    }
    if (m_kdeGlobal != nullptr) {
      wl_global_destroy(m_kdeGlobal);
    }
    if (m_gtkGlobal != nullptr) {
      wl_global_destroy(m_gtkGlobal);
    }
    if (m_dskGlobal != nullptr) {
      wl_global_destroy(m_dskGlobal);
    }
  }

  AppMenuRegistry::Entry& AppMenuRegistry::entryFor(wlr_surface* surface) {
    if (wlr_addon* addon = wlr_addon_find(&surface->addons, this, &AppMenuCallbacks::kAddon)) {
      Entry* existing = wl_container_of(addon, existing, addon);
      return *existing;
    }
    auto owned = std::make_unique<Entry>();
    owned->registry = this;
    owned->surface = surface;
    wlr_addon_init(&owned->addon, &surface->addons, this, &AppMenuCallbacks::kAddon);
    m_entries.push_back(std::move(owned));
    return *m_entries.back();
  }

  // The surface is gone: its protocol objects stay inert until the client destroys them.
  void AppMenuRegistry::destroy(Entry* entry) {
    wlr_addon_finish(&entry->addon);
    for (wl_resource* resource : entry->resources) {
      wl_resource_set_user_data(resource, nullptr);
    }
    if (entry->dskResource != nullptr) {
      wl_resource_set_user_data(entry->dskResource, nullptr);
    }
    const bool hadMenu = !std::holds_alternative<std::monostate>(entry->menu);
    std::erase_if(m_entries, [&](const auto& owned) { return owned.get() == entry; });
    if (hadMenu) {
      changed();
    }
  }

  void AppMenuRegistry::changed() {
    if (m_onChange) {
      m_onChange();
    }
  }

  const AppMenu* AppMenuRegistry::menuFor(const wlr_surface* surface) const {
    const auto found = std::ranges::find_if(m_entries, [&](const auto& owned) { return owned->surface == surface; });
    return found != m_entries.end() ? &(*found)->menu : nullptr;
  }

  bool AppMenuRegistry::activate(const wlr_surface* surface, uint64_t serial, uint32_t id) const {
    const auto found = std::ranges::find_if(m_entries, [&](const auto& owned) { return owned->surface == surface; });
    if (found == m_entries.end() || (*found)->dskResource == nullptr) {
      return false;
    }
    const auto* menu = std::get_if<DskMenu>(&(*found)->menu);
    if (menu == nullptr || menu->serial != serial) {
      return false;
    }
    const auto item = std::ranges::find_if(menu->items, [&](const DskMenuItem& candidate) { return candidate.id == id; });
    const bool parent =
        std::ranges::any_of(menu->items, [&](const DskMenuItem& candidate) { return candidate.parent == id; });
    constexpr uint32_t kInert = DSK_APP_MENU_V1_FLAGS_SEPARATOR | DSK_APP_MENU_V1_FLAGS_DISABLED;
    if (item == menu->items.end() || parent || (item->flags & kInert) != 0) {
      return false;
    }
    dsk_app_menu_v1_send_activated((*found)->dskResource, id);
    return true;
  }

} // namespace andromeda
