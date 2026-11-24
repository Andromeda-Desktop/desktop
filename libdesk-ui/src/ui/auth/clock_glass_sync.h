#pragma once

#include "ui/auth/auth_column.h"
#include "wayland/dsk_surface_client.h"
#include "wayland/surface.h"
#include "wayland/wayland_connection.h"

#include <memory>
#include <utility>

// Sends the clock's glass mask (AuthColumn::clockGlassMask) on its surface's own dsk object when it changes; the mask
// applies with the surface's next commit. A new surface (an editor opened again) has a new dsk object.
class ClockGlassSync {
public:
  void sync(WaylandConnection& connection, WaylandSurface& surface, AuthColumn::GlassMask mask) {
    if (&surface != m_surface) {
      m_dsk = nullptr;
      m_sent = {};
      m_surface = &surface;
    }
    if (mask == m_sent) {
      return;
    }
    if (m_dsk == nullptr) {
      if (mask.alpha.empty() || surface.dsk() == nullptr) {
        m_sent = std::move(mask);
        return;
      }
      m_dsk = surface.dsk();
    }
    (void)m_dsk->setGlassMask(
        connection.shm(), mask.alpha, mask.pixelWidth, mask.pixelHeight, mask.x, mask.y, mask.width, mask.height
    );
    m_sent = std::move(mask);
  }

private:
  WaylandSurface* m_surface = nullptr;
  DskSurfaceClient* m_dsk = nullptr; // m_surface's own
  AuthColumn::GlassMask m_sent;
};
