#include "wayland/toplevel_surface.h"

#include "core/log.h"
#include "dsk-app-menu-v1-client-protocol.h"
#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "wayland/dsk_surface_client.h"
#include "wayland/wayland_connection.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-foreign-unstable-v2-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace {

  constexpr Logger kLog("wayland");

  const xdg_surface_listener kXdgSurfaceListener = {
      .configure = &ToplevelSurface::handleXdgSurfaceConfigure,
  };

  const xdg_toplevel_listener kToplevelListener = {
      .configure = &ToplevelSurface::handleToplevelConfigure,
      .close = &ToplevelSurface::handleToplevelClose,
      .configure_bounds = &ToplevelSurface::handleToplevelConfigureBounds,
      .wm_capabilities = &ToplevelSurface::handleToplevelWmCapabilities,
  };

  // xdg_toplevel.set_title must be UTF-8 or the compositor ends the client; a title can come from untrusted bytes (a
  // terminal program, a file name). Each invalid sequence becomes U+FFFD.
  std::string validUtf8(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    const auto* p = reinterpret_cast<const unsigned char*>(in.data());
    const std::size_t n = in.size();
    for (std::size_t i = 0; i < n;) {
      const unsigned char c = p[i];
      std::size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
      bool ok = len > 0 && i + len <= n;
      for (std::size_t k = 1; ok && k < len; ++k) ok = (p[i + k] & 0xC0) == 0x80;
      if (ok && len > 1) {
        std::uint32_t cp = c & (0x7F >> len);
        for (std::size_t k = 1; k < len; ++k) cp = (cp << 6) | (p[i + k] & 0x3F);
        constexpr std::uint32_t kMin[] = {0, 0, 0x80, 0x800, 0x10000};
        ok = cp >= kMin[len] && cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF);
      }
      if (ok) {
        out.append(in, i, len);
        i += len;
      } else {
        out += "\xEF\xBF\xBD";
        ++i;
      }
    }
    return out;
  }
} // namespace

ToplevelSurface::ToplevelSurface(WaylandConnection& connection) : WaylandSurface(connection) {}

ToplevelSurface::~ToplevelSurface() {
  m_connection.unregisterSurface(m_surface);
  destroyRoleObjects();
}

bool ToplevelSurface::initialize(OutputId output, const ToplevelSurfaceConfig& config) {
  return initialize(m_connection.wlOutput(output), config);
}

bool ToplevelSurface::initialize(wl_output* output, const ToplevelSurfaceConfig& config) {
  if (!m_connection.hasXdgShell()) {
    kLog.warn("toplevel: missing xdg-shell");
    return false;
  }

  if (!createWlSurface()) {
    return false;
  }

  std::int32_t bufferScale = 1;
  if (output != nullptr) {
    if (const auto* wlOutput = m_connection.findOutputByWl(output); wlOutput != nullptr) {
      bufferScale = wlOutput->scale;
    }
    m_connection.registerSurfaceOutput(m_surface, output);
  }
  setBufferScale(bufferScale);

  m_pendingWidth = std::max<std::uint32_t>(1, config.width);
  m_pendingHeight = std::max<std::uint32_t>(1, config.height);
  m_floatingWidth = m_pendingWidth;
  m_resizeMargin = config.resizeMargin;
  m_floatingHeight = m_pendingHeight;
  m_lastToplevelWidth = 0;
  m_lastToplevelHeight = 0;

  m_xdgSurface = xdg_wm_base_get_xdg_surface(m_connection.xdgWmBase(), m_surface);
  if (m_xdgSurface == nullptr) {
    destroySurface();
    return false;
  }
  xdg_surface_add_listener(m_xdgSurface, &kXdgSurfaceListener, this);

  m_toplevel = xdg_surface_get_toplevel(m_xdgSurface);
  if (m_toplevel == nullptr) {
    destroyRoleObjects();
    destroySurface();
    return false;
  }

  xdg_toplevel_add_listener(m_toplevel, &kToplevelListener, this);
  // The window draws its own header, so the compositor must not add a title bar.
  if (auto* manager = m_connection.decorationManager(); manager != nullptr) {
    m_decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(manager, m_toplevel);
    zxdg_toplevel_decoration_v1_set_mode(m_decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
  }
  xdg_toplevel_set_title(m_toplevel, config.title.empty() ? "Noctalia" : validUtf8(config.title).c_str());
  setAccessibleTitle(config.title);
  xdg_toplevel_set_app_id(m_toplevel, config.appId != nullptr ? config.appId : "dev.noctalia.Noctalia");
  m_minWidth = config.minWidth;
  m_minHeight = config.minHeight;
  if (config.minWidth > 0 || config.minHeight > 0) {
    sendMinSize();
  }
  if (config.maxWidth > 0 || config.maxHeight > 0) {
    xdg_toplevel_set_max_size(
        m_toplevel, static_cast<std::int32_t>(config.maxWidth), static_cast<std::int32_t>(config.maxHeight)
    );
  }

  if (config.maximized) {
    xdg_toplevel_set_maximized(m_toplevel);
  }

  wl_surface_commit(m_surface);
  wl_display_flush(m_connection.display());
  if (wl_display_roundtrip(m_connection.display()) < 0) {
    kLog.warn("toplevel: initial roundtrip failed");
    destroyRoleObjects();
    destroySurface();
    return false;
  }

  setRunning(true);
  return true;
}

void ToplevelSurface::setClosedCallback(std::function<void()> callback) { m_closedCallback = std::move(callback); }

void ToplevelSurface::setMinSize(std::uint32_t minWidth, std::uint32_t minHeight) {
  m_minWidth = minWidth;
  m_minHeight = minHeight;
  sendMinSize();
}

void ToplevelSurface::sendMinSize() {
  if (m_toplevel == nullptr) {
    return;
  }
  const auto cap = [](std::uint32_t value, std::uint32_t bound) { return bound > 0 ? std::min(value, bound) : value; };
  xdg_toplevel_set_min_size(
      m_toplevel, static_cast<std::int32_t>(cap(m_minWidth, m_boundsWidth)),
      static_cast<std::int32_t>(cap(m_minHeight, m_boundsHeight))
  );
}

void ToplevelSurface::setTitle(const std::string& title) {
  if (m_toplevel != nullptr) {
    xdg_toplevel_set_title(m_toplevel, validUtf8(title).c_str());
  }
}

void ToplevelSurface::beginMove(std::uint32_t serial) {
  if (m_toplevel != nullptr) {
    xdg_toplevel_move(m_toplevel, m_connection.seat(), serial);
  }
}

void ToplevelSurface::beginResize(std::uint32_t serial, std::uint32_t edges) {
  if (m_toplevel != nullptr) {
    xdg_toplevel_resize(m_toplevel, m_connection.seat(), serial, edges);
  }
}

void ToplevelSurface::setMaximized(bool maximized) {
  if (m_toplevel == nullptr) {
    return;
  }
  if (maximized) {
    xdg_toplevel_set_maximized(m_toplevel);
  } else {
    xdg_toplevel_unset_maximized(m_toplevel);
  }
}

void ToplevelSurface::setFullscreen(bool fullscreen) {
  if (m_toplevel == nullptr) {
    return;
  }
  if (fullscreen) {
    xdg_toplevel_set_fullscreen(m_toplevel, nullptr);
  } else {
    xdg_toplevel_unset_fullscreen(m_toplevel);
  }
}

void ToplevelSurface::setMinimized() {
  if (m_toplevel != nullptr) {
    xdg_toplevel_set_minimized(m_toplevel);
  }
}

void ToplevelSurface::showWindowMenu(std::uint32_t serial, std::int32_t x, std::int32_t y) {
  if (m_toplevel != nullptr) {
    xdg_toplevel_show_window_menu(m_toplevel, m_connection.seat(), serial, x, y);
  }
}

bool ToplevelSurface::requestWindowSize(std::uint32_t width, std::uint32_t height) {
  if (m_xdgSurface == nullptr || m_windowWidth == 0 || m_state.maximized || m_state.fullscreen ||
      m_lastToplevelWidth > 0 || m_lastToplevelHeight > 0) {
    return false;
  }
  width = std::max<std::uint32_t>(1, width);
  height = std::max<std::uint32_t>(1, height);
  m_floatingWidth = width;
  m_floatingHeight = height;
  m_windowWidth = width;
  m_windowHeight = height;
  if (m_resizeMargin > 0) {
    const auto m = static_cast<std::int32_t>(m_margin);
    xdg_surface_set_window_geometry(m_xdgSurface, m, m, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height));
  }
  Surface::onConfigure(width + 2 * m_margin, height + 2 * m_margin);
  return true;
}

void ToplevelSurface::handleXdgSurfaceConfigure(void* data, xdg_surface* surface, std::uint32_t serial) {
  auto* self = static_cast<ToplevelSurface*>(data);
  xdg_surface_ack_configure(surface, serial);
  const ToplevelState& next = self->m_pendingState;
  // Tiled edges alone do not mean a layout-tiled window: some compositors send them to floating windows.
  const bool floating = !next.maximized && !next.fullscreen;

  std::uint32_t width = self->m_pendingWidth;
  std::uint32_t height = self->m_pendingHeight;
  // 0 x 0 = the client picks. After an unmaximize that is the last floating size, not the maximized one.
  if (floating && self->m_lastToplevelWidth <= 0 && self->m_floatingWidth > 0) {
    width = self->m_floatingWidth;
  }
  if (floating && self->m_lastToplevelHeight <= 0 && self->m_floatingHeight > 0) {
    height = self->m_floatingHeight;
  }
  if (self->m_lastToplevelWidth > 0) {
    width = static_cast<std::uint32_t>(self->m_lastToplevelWidth);
  }
  if (self->m_lastToplevelHeight > 0) {
    height = static_cast<std::uint32_t>(self->m_lastToplevelHeight);
  }
  width = std::max<std::uint32_t>(1, width);
  height = std::max<std::uint32_t>(1, height);
  if (floating) {
    self->m_floatingWidth = width;
    self->m_floatingHeight = height;
  }
  const bool stateChanged = next != self->m_state;
  self->m_state = next;
  self->m_margin = floating ? self->m_resizeMargin : 0;
  self->m_windowWidth = width;
  self->m_windowHeight = height;
  if (self->m_resizeMargin > 0) {
    const auto m = static_cast<std::int32_t>(self->m_margin);
    xdg_surface_set_window_geometry(
        surface, m, m, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)
    );
  }
  self->Surface::onConfigure(width + 2 * self->m_margin, height + 2 * self->m_margin);
  // After onConfigure, so the callback reads the size of this configure.
  if (stateChanged && self->m_stateCallback) {
    self->m_stateCallback(self->m_state);
  }
}

void ToplevelSurface::handleToplevelConfigure(
    void* data, xdg_toplevel* /*toplevel*/, std::int32_t width, std::int32_t height, wl_array* states
) {
  auto* self = static_cast<ToplevelSurface*>(data);
  ToplevelState next;
  if (states != nullptr) {
    const auto* begin = static_cast<const std::uint32_t*>(states->data);
    for (std::size_t i = 0; i < states->size / sizeof(std::uint32_t); ++i) {
      switch (begin[i]) {
      case XDG_TOPLEVEL_STATE_MAXIMIZED:
        next.maximized = true;
        break;
      case XDG_TOPLEVEL_STATE_FULLSCREEN:
        next.fullscreen = true;
        break;
      case XDG_TOPLEVEL_STATE_ACTIVATED:
        next.activated = true;
        break;
      case XDG_TOPLEVEL_STATE_TILED_LEFT:
      case XDG_TOPLEVEL_STATE_TILED_RIGHT:
      case XDG_TOPLEVEL_STATE_TILED_TOP:
      case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
        next.tiled = true;
        break;
      case XDG_TOPLEVEL_STATE_SUSPENDED:
        next.suspended = true;
        break;
      default:
        break;
      }
    }
  }
  self->m_pendingState = next;
  self->m_lastToplevelWidth = width;
  self->m_lastToplevelHeight = height;
  if (width > 0) {
    self->m_pendingWidth = static_cast<std::uint32_t>(width);
  }
  if (height > 0) {
    self->m_pendingHeight = static_cast<std::uint32_t>(height);
  }
}

void ToplevelSurface::handleToplevelClose(void* data, xdg_toplevel* /*toplevel*/) {
  auto* self = static_cast<ToplevelSurface*>(data);
  // A close request, not a close: an owner may refuse it (a confirm sheet) and keep drawing and taking input.
  if (self->m_closedCallback) {
    self->m_closedCallback();
  } else {
    self->setRunning(false);
  }
}

void ToplevelSurface::handleToplevelConfigureBounds(
    void* data, xdg_toplevel* /*toplevel*/, std::int32_t width, std::int32_t height
) {
  auto* self = static_cast<ToplevelSurface*>(data);
  const auto bound = [](std::int32_t v) { return static_cast<std::uint32_t>(std::max(v, 0)); };
  if (self->m_boundsWidth == bound(width) && self->m_boundsHeight == bound(height)) {
    return;
  }
  self->m_boundsWidth = bound(width);
  self->m_boundsHeight = bound(height);
  self->sendMinSize();
}

void ToplevelSurface::
    handleToplevelWmCapabilities(void* /*data*/, xdg_toplevel* /*toplevel*/, wl_array* /*capabilities*/) {}

void ToplevelSurface::destroyRoleObjects() {
  setParentWindow({});
  if (m_exported != nullptr) {
    zxdg_exported_v2_destroy(m_exported);
    m_exported = nullptr;
  }
  m_exportHandle.clear();
  m_exportReady = {};
  m_dsk.reset();
  setShortcutsInhibited(false);
  if (m_menuBar != nullptr) {
    dsk_app_menu_v1_destroy(m_menuBar);
    m_menuBar = nullptr;
  }
  m_menuActivated = {};
  if (m_decoration != nullptr) {
    zxdg_toplevel_decoration_v1_destroy(m_decoration);
    m_decoration = nullptr;
  }
  if (m_toplevel != nullptr) {
    xdg_toplevel_destroy(m_toplevel);
    m_toplevel = nullptr;
  }
  if (m_xdgSurface != nullptr) {
    xdg_surface_destroy(m_xdgSurface);
    m_xdgSurface = nullptr;
  }
}

namespace {
  constexpr std::string_view kPortalPrefix = "wayland:";
} // namespace

void ToplevelSurface::setParentWindow(std::string_view handle) {
  if (m_imported != nullptr) {
    zxdg_imported_v2_destroy(m_imported);
    m_imported = nullptr;
  }
  zxdg_importer_v2* importer = m_connection.foreignImporter();
  if (importer == nullptr || m_surface == nullptr || !handle.starts_with(kPortalPrefix) ||
      handle.size() == kPortalPrefix.size()) {
    return;
  }
  static constexpr zxdg_imported_v2_listener kListener{.destroyed = &ToplevelSurface::handleImportedDestroyed};
  const std::string exported(handle.substr(kPortalPrefix.size()));
  m_imported = zxdg_importer_v2_import_toplevel(importer, exported.c_str());
  zxdg_imported_v2_add_listener(m_imported, &kListener, this);
  zxdg_imported_v2_set_parent_of(m_imported, m_surface);
}

void ToplevelSurface::handleImportedDestroyed(void* data, zxdg_imported_v2* imported) {
  auto* self = static_cast<ToplevelSurface*>(data);
  if (self->m_imported == imported) {
    self->m_imported = nullptr;
  }
  zxdg_imported_v2_destroy(imported);
}

void ToplevelSurface::exportHandle(std::function<void(const std::string& handle)> ready) {
  zxdg_exporter_v2* exporter = m_connection.foreignExporter();
  if (exporter == nullptr || m_surface == nullptr) {
    ready({});
    return;
  }
  if (!m_exportHandle.empty()) {
    ready(m_exportHandle);
    return;
  }
  m_exportReady = std::move(ready);
  if (m_exported == nullptr) {
    static constexpr zxdg_exported_v2_listener kListener{.handle = &ToplevelSurface::handleExportedHandle};
    m_exported = zxdg_exporter_v2_export_toplevel(exporter, m_surface);
    zxdg_exported_v2_add_listener(m_exported, &kListener, this);
  }
}

void ToplevelSurface::handleExportedHandle(void* data, zxdg_exported_v2* /*exported*/, const char* handle) {
  auto* self = static_cast<ToplevelSurface*>(data);
  self->m_exportHandle = std::string(kPortalPrefix) + handle;
  if (auto ready = std::exchange(self->m_exportReady, {})) {
    ready(self->m_exportHandle);
  }
}

bool ToplevelSurface::setCaptureSecure(bool secure) {
  dsk_surface_manager_v1* manager = m_connection.dskSurfaceManager();
  if (manager == nullptr || m_surface == nullptr) {
    return false;
  }
  if (m_dsk == nullptr) {
    m_dsk = std::make_unique<DskSurfaceClient>(manager, m_surface);
  }
  return m_dsk->setSecure(secure);
}

void ToplevelSurface::setMenuBar(const std::vector<MenuBarItem>& items, std::function<void(std::uint32_t id)> activated) {
  dsk_app_menu_manager_v1* manager = m_connection.dskAppMenuManager();
  if (manager == nullptr || m_toplevel == nullptr || (m_menuBar == nullptr && items.empty())) {
    return;
  }
  if (m_menuBar == nullptr) {
    static constexpr dsk_app_menu_v1_listener kListener = {.activated = &ToplevelSurface::handleMenuActivated};
    m_menuBar = dsk_app_menu_manager_v1_get_menu(manager, m_toplevel);
    dsk_app_menu_v1_add_listener(m_menuBar, &kListener, this);
  }
  m_menuActivated = std::move(activated);
  for (const MenuBarItem& item : items) {
    dsk_app_menu_v1_item(m_menuBar, item.id, item.parent, item.enabled ? 0 : DSK_APP_MENU_V1_FLAGS_DISABLED,
                         item.label.c_str(), item.shortcut.c_str());
  }
  dsk_app_menu_v1_commit(m_menuBar);
}

void ToplevelSurface::handleMenuActivated(void* data, dsk_app_menu_v1* /*menu*/, std::uint32_t id) {
  auto* self = static_cast<ToplevelSurface*>(data);
  if (self->m_menuActivated) {
    self->m_menuActivated(id);
  }
}

void ToplevelSurface::setShortcutsInhibited(bool inhibited) {
  if (inhibited == (m_shortcutsInhibitor != nullptr)) {
    return;
  }
  if (!inhibited) {
    zwp_keyboard_shortcuts_inhibitor_v1_destroy(m_shortcutsInhibitor);
    m_shortcutsInhibitor = nullptr;
  } else if (m_surface != nullptr) {
    m_shortcutsInhibitor = m_connection.inhibitShortcuts(m_surface);
  }
}
