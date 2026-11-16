#include "render/text/text_glyph_atlas.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace {
  constexpr int kPad = 1; // empty texels around each glyph, so linear filtering never reads a neighbour
} // namespace

std::size_t TextGlyphAtlas::KeyHash::operator()(const Key& k) const noexcept {
  std::size_t h = std::hash<const void*>{}(k.font);
  h ^= (static_cast<std::size_t>(k.glyph) << 2U | static_cast<std::size_t>(k.phase)) + 0x9e3779b97f4a7c15ULL + (h << 6U)
       + (h >> 2U);
  return h;
}

TextGlyphAtlas::~TextGlyphAtlas() { forgetGlyphs(); }

void TextGlyphAtlas::forgetGlyphs() {
  for (auto& [key, slot] : m_glyphs) {
    cairo_scaled_font_destroy(key.font);
  }
  m_glyphs.clear();
  m_shelves.clear();
  m_nextShelfY = 0;
  ++m_generation;
}

void TextGlyphAtlas::clear(TextureManager* textures) {
  if (textures != nullptr && m_texture.id != 0) {
    textures->unload(m_texture);
  }
  m_texture = {};
  forgetGlyphs();
}

void TextGlyphAtlas::abandon() noexcept {
  m_texture = {};
  forgetGlyphs();
}

bool TextGlyphAtlas::place(int width, int height, int& x, int& y) {
  if (width > kSize || height > kSize) {
    return false;
  }
  // The lowest shelf that fits, wasting at most a quarter of its height.
  Shelf* best = nullptr;
  for (Shelf& shelf : m_shelves) {
    if (shelf.height >= height && shelf.height <= height + height / 4 + 2 && shelf.x + width <= kSize
        && (best == nullptr || shelf.height < best->height)) {
      best = &shelf;
    }
  }
  if (best == nullptr) {
    if (m_nextShelfY + height > kSize) {
      return false;
    }
    best = &m_shelves.emplace_back(Shelf{.y = m_nextShelfY, .height = height, .x = 0});
    m_nextShelfY += height;
  }
  x = best->x;
  y = best->y;
  best->x += width;
  return true;
}

const TextGlyphAtlas::Glyph*
TextGlyphAtlas::glyph(TextureManager& textures, cairo_scaled_font_t* font, std::uint32_t glyphIndex, int phase) {
  const Key key{.font = font, .glyph = glyphIndex, .phase = phase};
  if (auto it = m_glyphs.find(key); it != m_glyphs.end()) {
    return it->second.ink ? &it->second.glyph : nullptr;
  }
  if (m_texture.id == 0 || m_texture.generation != textures.generation()) {
    if (m_texture.id != 0) {
      forgetGlyphs();
    }
    // Zeroed, not createEmpty(): filtering at a glyph edge reads the texels between glyphs.
    const std::vector<std::uint8_t> zeros(static_cast<std::size_t>(kSize) * kSize, 0);
    m_texture = textures.loadFromPixels(zeros.data(), kSize, kSize, TextureDataFormat::Alpha, TextureFilter::Linear);
    if (m_texture.id == 0) {
      return nullptr;
    }
  }

  const double shift = static_cast<double>(phase) / kSubpixelSteps;
  cairo_glyph_t g{.index = glyphIndex, .x = 0.0, .y = 0.0};
  cairo_text_extents_t ext;
  cairo_scaled_font_glyph_extents(font, &g, 1, &ext);
  Slot slot;
  if (ext.width > 0.0 && ext.height > 0.0) {
    const int left = static_cast<int>(std::floor(ext.x_bearing + shift)) - kPad;
    const int top = static_cast<int>(std::floor(ext.y_bearing)) - kPad;
    const int width = static_cast<int>(std::ceil(ext.x_bearing + shift + ext.width)) + kPad - left;
    const int height = static_cast<int>(std::ceil(ext.y_bearing + ext.height)) + kPad - top;
    int x = 0;
    int y = 0;
    if (!place(width + kPad, height + kPad, x, y)) {
      // Full: start over. Entries that hold quads of the old page see the new generation and rebuild.
      clear(&textures);
      return glyph(textures, font, glyphIndex, phase);
    }
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_A8, width, height);
    cairo_t* cr = cairo_create(surface);
    cairo_set_scaled_font(cr, font);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
    g.x = shift - left;
    g.y = -top;
    cairo_show_glyphs(cr, &g, 1);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    const unsigned char* data = cairo_image_surface_get_data(surface);
    m_scratch.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int row = 0; row < height; ++row) {
      std::copy_n(data + static_cast<std::ptrdiff_t>(row) * stride, width, m_scratch.data() + row * width);
    }
    cairo_surface_destroy(surface);
    textures.updateSubImage(m_texture, m_scratch.data(), x, y, width, height, TextureDataFormat::Alpha);
    slot.ink = true;
    slot.glyph = Glyph{
        .x = static_cast<std::uint16_t>(x),
        .y = static_cast<std::uint16_t>(y),
        .width = static_cast<std::uint16_t>(width),
        .height = static_cast<std::uint16_t>(height),
        .left = static_cast<std::int16_t>(left),
        .top = static_cast<std::int16_t>(top),
    };
  }
  auto [it, inserted] = m_glyphs.emplace(Key{.font = cairo_scaled_font_reference(font), .glyph = glyphIndex, .phase = phase}, slot);
  return it->second.ink ? &it->second.glyph : nullptr;
}
