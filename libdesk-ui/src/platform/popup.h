#pragma once

#include "platform/output_id.h"
#include "platform/surface.h"
#include "ui/popup_positioner.h"

#include <cstdint>
#include <functional>

struct PopupSurfaceConfig {
  std::int32_t anchorX = 0;
  std::int32_t anchorY = 0;
  std::int32_t anchorWidth = 1;
  std::int32_t anchorHeight = 1;
  std::uint32_t width = 1;
  std::uint32_t height = 1;
  PopupAnchor anchor = PopupAnchor::None;
  PopupGravity gravity = PopupGravity::None;
  std::uint32_t constraintAdjustment = PopupConstraint::None;
  std::int32_t offsetX = 0;
  std::int32_t offsetY = 0;
  std::uint32_t serial = 0;
  bool grab = true;
  bool reactive = false;
};

// The surface a popup attaches to. width and height: the parent's size in logical pixels.
struct PopupParent {
  Surface* surface = nullptr;
  OutputId output = OutputId::None;
  std::uint32_t serial = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

// A popup (menu, tooltip, dropdown) placed relative to a parent surface. The platform subclass (Wayland:
// PopupSurface) owns the native popup.
class Popup : public virtual Surface {
public:
  using Surface::initialize;
  virtual bool initialize(const PopupParent& parent, PopupSurfaceConfig config) = 0;
  // When commit is false, the geometry change stays pending until the next rendered frame, so the window system
  // never scales the previous buffer to the new geometry.
  virtual bool resize(std::uint32_t width, std::uint32_t height, bool commit = true) = 0;
  virtual bool repositionAnchor(const PopupSurfaceConfig& anchorConfig, bool commit = true) = 0;
  virtual void setDismissedCallback(std::function<void()> callback) = 0;
  // Position relative to the parent from the last configure.
  [[nodiscard]] virtual std::int32_t configuredX() const noexcept = 0;
  [[nodiscard]] virtual std::int32_t configuredY() const noexcept = 0;
};
