#pragma once

#include "render/core/texture_manager.h"

#include <cairo.h>
#include <cstdint>
#include <unordered_map>
#include <vector>

// Glyph coverage for UI text: each (scaled font, glyph, x subpixel phase) is rasterized once by Cairo into one A8
// texture page, and labels draw quads from it tinted in the shader. A full page is cleared (generation() changes).
class TextGlyphAtlas {
public:
  static constexpr int kSize = 2048;
  static constexpr int kSubpixelSteps = 4; // x phases per pixel, as Cairo's image backend places glyphs

  // Device pixels. left/top place the bitmap relative to the pen position on the baseline (y down).
  struct Glyph {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::int16_t left = 0;
    std::int16_t top = 0;
  };

  TextGlyphAtlas() = default;
  ~TextGlyphAtlas();
  TextGlyphAtlas(const TextGlyphAtlas&) = delete;
  TextGlyphAtlas& operator=(const TextGlyphAtlas&) = delete;

  // Rasterizes and uploads on first use. Null for a glyph without ink, or when the page cannot be made.
  const Glyph* glyph(TextureManager& textures, cairo_scaled_font_t* font, std::uint32_t glyphIndex, int phase);
  [[nodiscard]] TextureId texture() const noexcept { return m_texture.id; }
  [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }
  // Frees the page (GPU context current). abandon() forgets it after the context was lost.
  void clear(TextureManager* textures);
  void abandon() noexcept;

private:
  struct Key {
    cairo_scaled_font_t* font;
    std::uint32_t glyph;
    int phase;
    bool operator==(const Key&) const noexcept = default;
  };
  struct KeyHash {
    std::size_t operator()(const Key& k) const noexcept;
  };
  struct Shelf {
    int y = 0;
    int height = 0;
    int x = 0;
  };
  struct Slot {
    Glyph glyph;
    bool ink = false;
  };

  // False when the page is full.
  bool place(int width, int height, int& x, int& y);
  void forgetGlyphs();

  TextureHandle m_texture;
  std::unordered_map<Key, Slot, KeyHash> m_glyphs; // each key holds a reference on its scaled font
  std::vector<Shelf> m_shelves;
  int m_nextShelfY = 0;
  std::uint64_t m_generation = 1;
  std::vector<std::uint8_t> m_scratch;
};
