#include "wayland/wayland_egl_window.h"

#include <wayland-egl.h>

WaylandEglWindow::~WaylandEglWindow() {
  if (m_window != nullptr) {
    wl_egl_window_destroy(m_window);
  }
}

void* WaylandEglWindow::resize(std::uint32_t bufferWidth, std::uint32_t bufferHeight) {
  if (m_window == nullptr) {
    m_window = wl_egl_window_create(m_surface, static_cast<int>(bufferWidth), static_cast<int>(bufferHeight));
  } else {
    wl_egl_window_resize(m_window, static_cast<int>(bufferWidth), static_cast<int>(bufferHeight), 0, 0);
  }
  return m_window;
}
