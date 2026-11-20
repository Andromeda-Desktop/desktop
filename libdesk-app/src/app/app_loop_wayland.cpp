#include "app/app_loop.h"

#include "wayland/wayland_connection.h"

// createDisplay() makes a WaylandConnection in Wayland builds.
WaylandConnection& AppLoop::wayland() noexcept { return static_cast<WaylandConnection&>(*m_display); }
