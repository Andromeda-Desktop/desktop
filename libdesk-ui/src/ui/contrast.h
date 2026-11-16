#pragma once

#include "render/core/color.h"

// OKLCH colour; hue in radians.
struct Oklch {
  double lightness = 0.0;
  double chroma = 0.0;
  double hueRadians = 0.0;
};

[[nodiscard]] Oklch toOklch(const Color& color);
// Opaque sRGB colour quantized to 8 bits; chroma drops until the colour is in gamut.
[[nodiscard]] Color fromOklch(const Oklch& color);
// WCAG contrast ratio of two opaque colours, in [1,21].
[[nodiscard]] double contrastRatio(const Color& a, const Color& b);
// Moves the foreground's OKLCH lightness toward white (preferLight > 0), black (< 0) or away from
// the background (0) until it meets minRatio; returns the best endpoint when no lightness does.
[[nodiscard]] Color ensureContrast(const Color& foreground, const Color& background, double minRatio = 4.5,
                                   int preferLight = 0);
