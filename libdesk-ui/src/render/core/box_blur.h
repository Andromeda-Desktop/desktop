#pragma once

#include <cstdint>
#include <vector>

// Approximate Gaussian blur (three box passes of radius / 2) of a tightly packed RGBA buffer, in place. The blur
// reaches about 1.5 x radius from the source ink.
void blurRgba(std::vector<std::uint8_t>& rgba, int width, int height, int radius);
// Same as blurRgba for a tightly packed 8-bit coverage buffer, then scaled so its densest pixel is opaque: thin and
// bold strokes cast an equally dense shadow. `wipe` clears the scratch copy (sensitive text).
// Raster px a blurAlpha of `radius` reaches past the source ink.
[[nodiscard]] constexpr int blurReach(int radius) noexcept { return radius > 0 ? 3 * (radius / 2 > 1 ? radius / 2 : 1) : 0; }
void blurAlpha(std::vector<std::uint8_t>& alpha, int width, int height, int radius, bool wipe = false);
