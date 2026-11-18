#pragma once

#include <cstdint>

// Values equal wl_pointer.axis and wl_pointer.axis_source, so the Wayland backend passes them through unchanged.
namespace PointerAxis {
  inline constexpr std::uint32_t Vertical = 0;
  inline constexpr std::uint32_t Horizontal = 1;
} // namespace PointerAxis

namespace AxisSource {
  inline constexpr std::uint32_t Wheel = 0;
  inline constexpr std::uint32_t Finger = 1;
  inline constexpr std::uint32_t Continuous = 2;
  inline constexpr std::uint32_t WheelTilt = 3;
} // namespace AxisSource
