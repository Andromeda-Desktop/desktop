#include "render/core/box_blur.h"

#include "util/secure_wipe.h"

#include <algorithm>
#include <cstddef>

namespace {

  // One box pass along x or y; samples past an edge repeat the edge sample.
  void boxPass(
      const std::vector<std::uint8_t>& source, std::vector<std::uint8_t>& destination, int width, int height,
      int channels, bool vertical, int radius
  ) {
    const auto diameter = static_cast<std::uint32_t>(radius * 2 + 1);
    const int length = vertical ? height : width;
    const int lines = vertical ? width : height;
    const auto ch = static_cast<std::size_t>(channels);
    const std::size_t step = vertical ? static_cast<std::size_t>(width) * ch : ch;
    const std::size_t lineStride = vertical ? ch : static_cast<std::size_t>(width) * ch;
    for (int line = 0; line < lines; ++line) {
      for (std::size_t channel = 0; channel < ch; ++channel) {
        const std::size_t base = static_cast<std::size_t>(line) * lineStride + channel;
        const auto at = [&](int i) { return base + static_cast<std::size_t>(std::clamp(i, 0, length - 1)) * step; };
        std::uint32_t sum = 0;
        for (int offset = -radius; offset <= radius; ++offset) {
          sum += source[at(offset)];
        }
        for (int i = 0; i < length; ++i) {
          destination[at(i)] = static_cast<std::uint8_t>((sum + diameter / 2U) / diameter);
          sum = sum - source[at(i - radius)] + source[at(i + radius + 1)];
        }
      }
    }
  }

  void blur(std::vector<std::uint8_t>& pixels, int width, int height, int channels, int radius, bool wipe) {
    if (pixels.empty() || width <= 0 || height <= 0 || radius <= 0) {
      return;
    }
    const int passRadius = std::max(1, radius / 2);
    std::vector<std::uint8_t> scratch(pixels.size());
    constexpr int kBlurPasses = 3;
    for (int pass = 0; pass < kBlurPasses; ++pass) {
      boxPass(pixels, scratch, width, height, channels, false, passRadius);
      boxPass(scratch, pixels, width, height, channels, true, passRadius);
    }
    if (wipe) {
      util::secureWipe(scratch.data(), scratch.size());
    }
  }

} // namespace

void blurRgba(std::vector<std::uint8_t>& rgba, int width, int height, int radius) {
  blur(rgba, width, height, 4, radius, false);
}

void blurAlpha(std::vector<std::uint8_t>& alpha, int width, int height, int radius, bool wipe) {
  blur(alpha, width, height, 1, radius, wipe);
  const std::uint8_t peak = alpha.empty() ? 0 : *std::ranges::max_element(alpha);
  if (peak == 0 || peak == 255) {
    return;
  }
  for (std::uint8_t& a : alpha) {
    a = static_cast<std::uint8_t>((static_cast<std::uint32_t>(a) * 255U + peak / 2U) / peak);
  }
}
