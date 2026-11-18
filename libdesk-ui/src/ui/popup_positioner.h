#pragma once

#include <cstdint>

// Where a popup attaches to its anchor rectangle, and which way it grows from there. Values match xdg_positioner.
enum class PopupAnchor : std::uint32_t {
  None = 0,
  Top = 1,
  Bottom = 2,
  Left = 3,
  Right = 4,
  TopLeft = 5,
  BottomLeft = 6,
  TopRight = 7,
  BottomRight = 8,
};

enum class PopupGravity : std::uint32_t {
  None = 0,
  Top = 1,
  Bottom = 2,
  Left = 3,
  Right = 4,
  TopLeft = 5,
  BottomLeft = 6,
  TopRight = 7,
  BottomRight = 8,
};

// How the platform may move a popup that does not fit on the output: a bit set.
namespace PopupConstraint {
  inline constexpr std::uint32_t None = 0;
  inline constexpr std::uint32_t SlideX = 1;
  inline constexpr std::uint32_t SlideY = 2;
  inline constexpr std::uint32_t FlipX = 4;
  inline constexpr std::uint32_t FlipY = 8;
} // namespace PopupConstraint
