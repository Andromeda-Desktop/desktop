#pragma once

#include "render/core/mat3.h"
#include "render/core/texture_handle.h"

#include <cstdint>
#include <span>
#include <vector>

// Parts of a grid draw, bottom up: the background fill, cell backgrounds (rects), text (glyphs, then over).
enum TerminalGridPhase : std::uint8_t {
  kTerminalGridFill = 1 << 0,
  kTerminalGridBackgrounds = 1 << 1,
  kTerminalGridText = 1 << 2,
  kTerminalGridAll = kTerminalGridFill | kTerminalGridBackgrounds | kTerminalGridText,
};

// One frame of a terminal cell grid. Each vector is one triangle list; positions are logical px from the grid origin.
struct TerminalGridDraw {
  float width = 0.0F; // node size; the background fills it
  float height = 0.0F;
  float background[4] = {0.0F, 0.0F, 0.0F, 1.0F}; // premultiplied rgba
  TextureId atlas;
  int atlasWidth = 0;
  int atlasHeight = 0;
  std::span<const std::vector<float>> rects;  // per vertex: x, y, r, g, b, a (premultiplied)
  std::span<const std::vector<float>> glyphs; // per vertex: x, y, u, v (atlas px), r, g, b, a (premultiplied)
  std::span<const std::vector<float>> over;   // drawn after the glyphs (cursor); rect layout
  std::span<const std::vector<float>> overGlyphs;
  float surfaceWidth = 0.0F;
  float surfaceHeight = 0.0F;
  Mat3 transform = Mat3::identity();     // the fill
  Mat3 cellTransform = Mat3::identity(); // rects and glyphs: transform with the grid origin
  float opacity = 1.0F;
  std::uint8_t phases = kTerminalGridAll; // TerminalGridPhase bits to draw
};
