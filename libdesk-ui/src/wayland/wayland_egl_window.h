#pragma once

#include "render/backend/native_egl_window.h"

struct wl_egl_window;
struct wl_surface;

class WaylandEglWindow final : public NativeEglWindow {
public:
  explicit WaylandEglWindow(wl_surface* surface) : m_surface(surface) {}
  ~WaylandEglWindow() override;

  WaylandEglWindow(const WaylandEglWindow&) = delete;
  WaylandEglWindow& operator=(const WaylandEglWindow&) = delete;

  [[nodiscard]] void* resize(std::uint32_t bufferWidth, std::uint32_t bufferHeight) override;

private:
  wl_surface* m_surface;
  wl_egl_window* m_window = nullptr;
};
