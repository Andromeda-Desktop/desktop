#pragma once

#include "platform/popup.h"

#include <cstdint>

struct PopupBox {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t width = 0;
  std::int32_t height = 0;
};

// Places a popup by the xdg_positioner rules for a window system that has none: anchor point, gravity and offset,
// then flip and slide into `bounds` as config.constraintAdjustment allows. The anchor rect is relative to
// (parentX, parentY); the result shares the space of `bounds`.
[[nodiscard]] PopupBox placePopup(
    const PopupSurfaceConfig& config, std::int32_t parentX, std::int32_t parentY, const PopupBox& bounds
);
