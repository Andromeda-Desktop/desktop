#pragma once

#include "platform/toplevel.h"
#include "wayland/surface.h"

#include <cstdint>
#include <functional>
#include <string>

struct wl_array;
struct wl_output;
struct xdg_toplevel;
struct zxdg_toplevel_decoration_v1;

class ToplevelSurface : public WaylandSurface, public Toplevel {
public:
  explicit ToplevelSurface(WaylandConnection& connection);
  ~ToplevelSurface() override;

  bool initialize() override { return false; }
  bool initialize(OutputId output, const ToplevelSurfaceConfig& config) override;
  bool initialize(wl_output* output, const ToplevelSurfaceConfig& config);

  void setClosedCallback(std::function<void()> callback) override;
  void setMinSize(std::uint32_t minWidth, std::uint32_t minHeight) override;
  void setTitle(const std::string& title) override;
  void beginMove(std::uint32_t serial) override;
  void beginResize(std::uint32_t serial, std::uint32_t edges) override;
  void setMaximized(bool maximized) override;
  void setFullscreen(bool fullscreen) override;
  void setMinimized() override;
  bool requestWindowSize(std::uint32_t width, std::uint32_t height) override;
  void showWindowMenu(std::uint32_t serial, std::int32_t x, std::int32_t y) override;
  void setStateCallback(std::function<void(const ToplevelState&)> callback) override {
    m_stateCallback = std::move(callback);
  }
  [[nodiscard]] const ToplevelState& state() const noexcept override { return m_state; }
  [[nodiscard]] std::uint32_t margin() const noexcept override { return m_margin; }
  [[nodiscard]] std::uint32_t windowWidth() const noexcept override { return m_windowWidth; }
  [[nodiscard]] std::uint32_t windowHeight() const noexcept override { return m_windowHeight; }

  [[nodiscard]] xdg_surface* xdgSurface() const noexcept override { return m_xdgSurface; }

  static void handleXdgSurfaceConfigure(void* data, xdg_surface* surface, std::uint32_t serial);
  static void handleToplevelConfigure(
      void* data, xdg_toplevel* toplevel, std::int32_t width, std::int32_t height, wl_array* states
  );
  static void handleToplevelClose(void* data, xdg_toplevel* toplevel);
  static void
  handleToplevelConfigureBounds(void* data, xdg_toplevel* toplevel, std::int32_t width, std::int32_t height);
  static void handleToplevelWmCapabilities(void* data, xdg_toplevel* toplevel, wl_array* capabilities);

private:
  void destroyRoleObjects();

  xdg_surface* m_xdgSurface = nullptr;
  xdg_toplevel* m_toplevel = nullptr;
  zxdg_toplevel_decoration_v1* m_decoration = nullptr;
  std::function<void()> m_closedCallback;
  std::function<void(const ToplevelState&)> m_stateCallback;
  ToplevelState m_state;
  ToplevelState m_pendingState;
  std::uint32_t m_floatingWidth = 0; // last size with no maximized or fullscreen state
  std::uint32_t m_floatingHeight = 0;
  std::uint32_t m_resizeMargin = 0;
  std::uint32_t m_margin = 0;
  std::uint32_t m_minWidth = 0;
  std::uint32_t m_minHeight = 0;
  // configure_bounds, 0 when the compositor sends none; the minimum size never exceeds it.
  std::uint32_t m_boundsWidth = 0;
  std::uint32_t m_boundsHeight = 0;
  void sendMinSize();
  std::uint32_t m_windowWidth = 0;
  std::uint32_t m_windowHeight = 0;
  std::uint32_t m_pendingWidth = 0;
  std::uint32_t m_pendingHeight = 0;
  std::int32_t m_lastToplevelWidth = 0;
  std::int32_t m_lastToplevelHeight = 0;
};
