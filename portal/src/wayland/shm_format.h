#pragma once

#include <cstdint>
#include <drm_fourcc.h>
#include <optional>

namespace xdpu {
  // Byte index of R, G and B inside one little-endian 32-bit pixel.
  struct RgbOffsets {
    int r;
    int g;
    int b;
  };

  // Empty for every format the capture paths cannot read.
  inline std::optional<RgbOffsets> rgbOffsets(uint32_t drmFormat) {
    switch (drmFormat) {
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_ARGB8888:
      return RgbOffsets{.r = 2, .g = 1, .b = 0};
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_ABGR8888:
      return RgbOffsets{.r = 0, .g = 1, .b = 2};
    default:
      return std::nullopt;
    }
  }
} // namespace xdpu
