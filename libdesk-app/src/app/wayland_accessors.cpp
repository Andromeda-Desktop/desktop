#include "app/app_loop.h"
#include "app/app_window.h"

#include "wayland/surface.h"
#include "wayland/wayland_connection.h"

// Wayland builds only: createDisplay() makes a WaylandConnection, and every Toplevel is a WaylandSurface.
WaylandConnection& AppLoop::wayland() noexcept { return static_cast<WaylandConnection&>(*m_display); }

wl_surface* AppWindow::wlSurface() const noexcept {
  return m_surface != nullptr ? dynamic_cast<const WaylandSurface&>(*m_surface).wlSurface() : nullptr;
}
