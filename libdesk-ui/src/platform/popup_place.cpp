#include "platform/popup_place.h"

namespace {
  // -1: left or top, 0: centre, 1: right or bottom. Anchor and gravity share the numbering.
  int horizontal(std::uint32_t edge) {
    switch (edge) {
    case 3:
    case 5:
    case 6:
      return -1;
    case 4:
    case 7:
    case 8:
      return 1;
    default:
      return 0;
    }
  }

  int vertical(std::uint32_t edge) {
    switch (edge) {
    case 1:
    case 5:
    case 7:
      return -1;
    case 2:
    case 6:
    case 8:
      return 1;
    default:
      return 0;
    }
  }

  struct Axis {
    std::int32_t anchorStart;
    std::int32_t anchorSize;
    std::int32_t size;
    std::int32_t offset;
    std::int32_t boundStart;
    std::int32_t boundSize;
  };

  std::int32_t start(const Axis& a, int anchor, int gravity) {
    const std::int32_t point = a.anchorStart + (anchor + 1) * a.anchorSize / 2;
    return point + a.offset + (gravity < 0 ? -a.size : gravity == 0 ? -a.size / 2 : 0);
  }

  bool fits(const Axis& a, std::int32_t s) { return s >= a.boundStart && s + a.size <= a.boundStart + a.boundSize; }

  std::int32_t place(const Axis& a, int anchor, int gravity, bool flip, bool slide) {
    std::int32_t s = start(a, anchor, gravity);
    if (fits(a, s)) {
      return s;
    }
    if (flip) {
      Axis flipped = a;
      flipped.offset = -a.offset;
      if (const std::int32_t f = start(flipped, -anchor, -gravity); fits(a, f)) {
        return f;
      }
    }
    if (slide) {
      if (s + a.size > a.boundStart + a.boundSize) {
        s = a.boundStart + a.boundSize - a.size;
      }
      if (s < a.boundStart) {
        s = a.boundStart;
      }
    }
    return s;
  }
} // namespace

PopupBox placePopup(const PopupSurfaceConfig& config, std::int32_t parentX, std::int32_t parentY, const PopupBox& bounds) {
  const auto width = static_cast<std::int32_t>(config.width);
  const auto height = static_cast<std::int32_t>(config.height);
  const auto anchor = static_cast<std::uint32_t>(config.anchor);
  const auto gravity = static_cast<std::uint32_t>(config.gravity);
  const std::uint32_t adjust = config.constraintAdjustment;
  const Axis x{parentX + config.anchorX, config.anchorWidth, width, config.offsetX, bounds.x, bounds.width};
  const Axis y{parentY + config.anchorY, config.anchorHeight, height, config.offsetY, bounds.y, bounds.height};
  return {
      .x = place(x, horizontal(anchor), horizontal(gravity), (adjust & PopupConstraint::FlipX) != 0,
                 (adjust & PopupConstraint::SlideX) != 0),
      .y = place(y, vertical(anchor), vertical(gravity), (adjust & PopupConstraint::FlipY) != 0,
                 (adjust & PopupConstraint::SlideY) != 0),
      .width = width,
      .height = height,
  };
}
