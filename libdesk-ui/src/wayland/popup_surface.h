#pragma once

#include "platform/popup.h"
#include "wayland/surface.h"

#include <cstdint>
#include <functional>
#include <memory>

struct wl_output;
struct xdg_popup;

class PopupSurface : public WaylandSurface, public Popup {
public:
  explicit PopupSurface(WaylandConnection& connection);
  ~PopupSurface() override;

  bool initialize() override { return false; }
  // Attaches to the parent's layer or xdg surface; false when the parent has neither.
  bool initialize(const PopupParent& parent, PopupSurfaceConfig config) override;
  bool initialize(zwlr_layer_surface_v1* parentLayerSurface, wl_output* output, PopupSurfaceConfig config);
  bool initializeAsChild(xdg_surface* parentXdgSurface, wl_output* output, PopupSurfaceConfig config);
  bool resize(std::uint32_t width, std::uint32_t height, bool commit = true) override;
  bool repositionAnchor(const PopupSurfaceConfig& anchorConfig, bool commit = true) override;

  void setDismissedCallback(std::function<void()> callback) override;

  [[nodiscard]] xdg_surface* xdgSurface() const noexcept override { return m_xdgSurface; }
  [[nodiscard]] std::int32_t configuredX() const noexcept override { return m_configuredX; }
  [[nodiscard]] std::int32_t configuredY() const noexcept override { return m_configuredY; }

  static void handleXdgSurfaceConfigure(void* data, xdg_surface* surface, std::uint32_t serial);
  static void handlePopupConfigure(
      void* data, xdg_popup* popup, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height
  );
  static void handlePopupDone(void* data, xdg_popup* popup);
  static void handlePopupRepositioned(void* data, xdg_popup* popup, std::uint32_t token);

private:
  void destroyRoleObjects();
  // Decide whether to take xdg_popup_grab vs. enroll in the active focus_grab
  // host. Sets m_enrolledInGrabHost when we registered with the host.
  void wireGrab();
  void unenrollFromGrabHost();

  PopupSurfaceConfig m_config;
  xdg_surface* m_xdgSurface = nullptr;
  xdg_popup* m_popup = nullptr;
  std::function<void()> m_dismissedCallback;
  std::uint32_t m_pendingWidth = 0;
  std::uint32_t m_pendingHeight = 0;
  std::uint32_t m_repositionToken = 0;
  std::int32_t m_configuredX = 0;
  std::int32_t m_configuredY = 0;
  bool m_enrolledInGrabHost = false;
  // Set false by the destructor. The init roundtrip re-enters event dispatch and can
  // destroy this popup mid-init; a captured copy of this token lets init detect that
  // and avoid touching freed `this`.
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
