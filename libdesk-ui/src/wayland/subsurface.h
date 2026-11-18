#pragma once

#include "platform/subsurface.h"
#include "wayland/surface.h"

#include <cstdint>

struct wl_output;
struct wl_subsurface;
struct wl_surface;

class WaylandSubsurface : public WaylandSurface, public Subsurface {
public:
  explicit WaylandSubsurface(WaylandConnection& connection);
  ~WaylandSubsurface() override;

  bool initialize() override;
  bool initialize(const Surface& parent, SubsurfaceConfig config) override;
  bool initialize(wl_surface* parentSurface, wl_output* output);

  void setPosition(std::int32_t x, std::int32_t y) override;
  void resize(std::uint32_t width, std::uint32_t height) override;
  void setStacking(SubsurfaceStacking stacking) override;

private:
  void applyStacking();

  SubsurfaceConfig m_config;
  wl_surface* m_parentSurface = nullptr;
  wl_output* m_output = nullptr;
  wl_subsurface* m_subsurface = nullptr;
};
