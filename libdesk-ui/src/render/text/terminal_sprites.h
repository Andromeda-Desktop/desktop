#pragma once

#include <cstdint>
#include <vector>

// Box drawing (U+2500-257F), block elements (U+2580-259F), braille (U+2800-28FF) and powerline (U+E0B0-E0BF)
// drawn to the cell, so lines and blocks meet across cells whatever the font does.
namespace terminal_sprites {

  [[nodiscard]] bool covers(char32_t codepoint) noexcept;

  // Fills alpha (width * height coverage bytes, row-major) for one cell; false when codepoint is not a sprite.
  // stroke: the light line width in pixels.
  bool draw(char32_t codepoint, int width, int height, int stroke, std::vector<std::uint8_t>& alpha);

} // namespace terminal_sprites
