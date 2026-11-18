#pragma once

#include "platform/surface.h"

#include <cstdint>

enum class SubsurfaceStacking {
  AboveParent,
  BelowParent,
};

// x, y: the position in the parent surface, logical pixels.
struct SubsurfaceConfig {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::int32_t x = 0;
  std::int32_t y = 0;
  SubsurfaceStacking stacking = SubsurfaceStacking::AboveParent;
  bool desynchronized = true;
};

// A child surface that moves with its parent. The platform subclass (Wayland: WaylandSubsurface) owns the native
// child; destroy it before its parent.
class Subsurface : public virtual Surface {
public:
  using Surface::initialize;
  virtual bool initialize(const Surface& parent, SubsurfaceConfig config) = 0;

  virtual void setPosition(std::int32_t x, std::int32_t y) = 0;
  virtual void resize(std::uint32_t width, std::uint32_t height) = 0;
  virtual void setStacking(SubsurfaceStacking stacking) = 0;
};
