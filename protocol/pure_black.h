#pragma once

// Rulebook §4.3 pure black, shared by libdesk-ui (shell and app palettes) and the compositor (its own chrome): in a
// dark scheme each opaque plane and glass tint keeps its CIE L* distance from `view`, and `view` sits on black.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace pure_black {

  [[nodiscard]] inline double linear(double channel) {
    return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
  }

  [[nodiscard]] inline double encode(double linearChannel) {
    return linearChannel <= 0.0031308 ? linearChannel * 12.92 : 1.055 * std::pow(linearChannel, 1.0 / 2.4) - 0.055;
  }

  // CIE L* (0..100) of an sRGB colour, channels 0..1.
  [[nodiscard]] inline double lightness(double r, double g, double b) {
    const double y = 0.2126 * linear(r) + 0.7152 * linear(g) + 0.0722 * linear(b);
    return y > 216.0 / 24389.0 ? 116.0 * std::cbrt(y) - 16.0 : y * 24389.0 / 27.0;
  }

  // The colour (sRGB 0..1) lowered by `shift` L*, scaled in linear light so hue and chroma ratio stay; quantized to
  // 8 bits. A colour at or below `shift` becomes black.
  [[nodiscard]] inline std::array<double, 3> lowered(double r, double g, double b, double shift) {
    const double from = lightness(r, g, b);
    const double to = from - shift;
    if (to <= 0.0 || from <= 0.0) {
      return {0.0, 0.0, 0.0};
    }
    const auto y = [](double l) { return l > 8.0 ? std::pow((l + 16.0) / 116.0, 3.0) : l * 27.0 / 24389.0; };
    const double scale = y(to) / y(from);
    const auto channel = [scale](double c) { return std::round(std::clamp(encode(linear(c) * scale), 0.0, 1.0) * 255.0) / 255.0; };
    return {channel(r), channel(g), channel(b)};
  }

  // A packed 0xRRGGBBAA token lowered by `shift` L*; alpha stays.
  [[nodiscard]] inline std::uint32_t loweredToken(std::uint32_t rgba, double shift) {
    const auto at = [rgba](int bits) { return static_cast<double>((rgba >> bits) & 0xFFU) / 255.0; };
    const auto out = lowered(at(24), at(16), at(8), shift);
    const auto byte = [](double c) { return static_cast<std::uint32_t>(std::lround(c * 255.0)); };
    return (byte(out[0]) << 24U) | (byte(out[1]) << 16U) | (byte(out[2]) << 8U) | (rgba & 0xFFU);
  }

  // The L* of a packed 0xRRGGBBAA token: the shift that puts it on black.
  [[nodiscard]] inline double tokenLightness(std::uint32_t rgba) {
    const auto at = [rgba](int bits) { return static_cast<double>((rgba >> bits) & 0xFFU) / 255.0; };
    return lightness(at(24), at(16), at(8));
  }

} // namespace pure_black
