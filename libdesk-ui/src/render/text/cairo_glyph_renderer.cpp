#include "render/text/cairo_glyph_renderer.h"

#include "render/backend/render_backend.h"
#include "render/core/box_blur.h"
#include "render/core/texture_manager.h"

#include <cairo-ft.h>
#include <cairo.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

  constexpr std::uint32_t kSizeQuant = 64;
  constexpr std::uint32_t kScaleQuant = 64;
  constexpr float kAxisAlignedEpsilon = 0.0001F;
  constexpr std::size_t kMaxMetrics = 1024;

  inline std::uint32_t quantizeSize(float v) {
    return static_cast<std::uint32_t>(std::max(0.0F, v) * static_cast<float>(kSizeQuant) + 0.5F);
  }

  inline std::uint16_t quantizeScale(float v) {
    return static_cast<std::uint16_t>(std::max(0.0F, v) * static_cast<float>(kScaleQuant) + 0.5F);
  }

  bool isAxisAligned(const Mat3& transform) {
    return std::abs(transform.m[1]) <= kAxisAlignedEpsilon && std::abs(transform.m[3]) <= kAxisAlignedEpsilon;
  }

  float snapToBufferPixel(float value, float scale) {
    const float safeScale = std::max(1.0F, scale);
    return std::round(value * safeScale) / safeScale;
  }

  void hashCombine(std::size_t& seed, std::size_t v) { seed ^= v + 0x9E3779B97F4A7C15ULL + (seed << 12) + (seed >> 4); }

  // Fixed salt mixed into the glyph cache key hash. std::hash<integral> is the
  // identity map on most libstdc++/libc++ builds, which clusters sequential
  // codepoints into adjacent buckets and degrades the open-addressing probe
  // sequence; seeding from a non-trivial constant decorrelates the low bits.
  constexpr std::size_t kGlyphHashSalt = 0x7E4B2A9C5D3F8161ULL;

  // Hinting is disabled for icons: tabler glyphs are monoline strokes with
  // fractional widths by design. Autohinter snaps each stroke to the nearest
  // integer pixel, which visibly thins the icons. Grayscale AA without
  // hinting preserves the intended stroke thickness.
  cairo_scaled_font_t*
  create_scaled_font(cairo_font_face_t* face, cairo_font_options_t* fontOptions, float rasterSize) {
    cairo_matrix_t fontMatrix;
    cairo_matrix_init_scale(&fontMatrix, rasterSize, rasterSize);
    cairo_matrix_t ctm;
    cairo_matrix_init_identity(&ctm);
    return cairo_scaled_font_create(face, &fontMatrix, &ctm, fontOptions);
  }

  CairoGlyphRenderer::TextMetrics metrics_from_extents(const cairo_text_extents_t& extents, float invScale) {
    return CairoGlyphRenderer::TextMetrics{
        .width = static_cast<float>(extents.x_advance) * invScale,
        .left = static_cast<float>(extents.x_bearing) * invScale,
        .right = static_cast<float>(extents.x_bearing + extents.width) * invScale,
        .top = static_cast<float>(extents.y_bearing) * invScale,
        .bottom = static_cast<float>(extents.y_bearing + extents.height) * invScale,
    };
  }

} // namespace

bool CairoGlyphRenderer::CacheKey::operator==(const CacheKey& other) const noexcept {
  return codepoint == other.codepoint && sizeQ == other.sizeQ && scaleQ == other.scaleQ && blurPx == other.blurPx;
}

std::size_t CairoGlyphRenderer::CacheKeyHash::operator()(const CacheKey& k) const noexcept {
  std::size_t seed = kGlyphHashSalt;
  hashCombine(seed, std::hash<char32_t>{}(k.codepoint));
  hashCombine(seed, std::hash<std::uint32_t>{}(k.sizeQ));
  hashCombine(seed, std::hash<std::uint16_t>{}(k.scaleQ));
  hashCombine(seed, std::hash<std::uint16_t>{}(k.blurPx));
  return seed;
}

CairoGlyphRenderer::CairoGlyphRenderer()
    : m_cache(
          {.entries = 512, .bytes = 8 * 1024 * 1024}, [](const CacheEntry& e) { return e.bytes; },
          [this](CacheKey&, CacheEntry& e) {
            if (m_textureManager != nullptr) {
              m_textureManager->unload(e.texture);
            }
          }
      ) {}
CairoGlyphRenderer::~CairoGlyphRenderer() { cleanup(); }

void CairoGlyphRenderer::initialize(const std::string& fontPath, RenderBackend* backend, TextureManager* textures) {
  m_backend = backend;
  m_textureManager = textures;

  if (FT_Init_FreeType(&m_ftLibrary) != 0) {
    throw std::runtime_error("CairoGlyphRenderer: FT_Init_FreeType failed");
  }
  if (FT_New_Face(m_ftLibrary, fontPath.c_str(), 0, &m_face) != 0) {
    cleanup();
    throw std::runtime_error("CairoGlyphRenderer: failed to load icon font: " + fontPath);
  }

  m_cairoFace = cairo_ft_font_face_create_for_ft_face(m_face, 0);
  if (m_cairoFace == nullptr || cairo_font_face_status(m_cairoFace) != CAIRO_STATUS_SUCCESS) {
    cleanup();
    throw std::runtime_error("CairoGlyphRenderer: cairo_ft_font_face_create_for_ft_face failed");
  }

  m_fontOptions = cairo_font_options_create();
  cairo_font_options_set_antialias(m_fontOptions, CAIRO_ANTIALIAS_GRAY);
  cairo_font_options_set_hint_style(m_fontOptions, CAIRO_HINT_STYLE_NONE);
  cairo_font_options_set_hint_metrics(m_fontOptions, CAIRO_HINT_METRICS_OFF);
}

void CairoGlyphRenderer::invalidateGlyphTextures() { m_cache.clear(); }

void CairoGlyphRenderer::abandonGlyphTextures() noexcept { m_cache.clear(nullptr); }

void CairoGlyphRenderer::cleanup() {
  m_cache.clear();
  m_metrics.clear();

  if (m_fontOptions != nullptr) {
    cairo_font_options_destroy(m_fontOptions);
    m_fontOptions = nullptr;
  }
  if (m_cairoFace != nullptr) {
    cairo_font_face_destroy(m_cairoFace);
    m_cairoFace = nullptr;
  }
  if (m_face != nullptr) {
    FT_Done_Face(m_face);
    m_face = nullptr;
  }
  if (m_ftLibrary != nullptr) {
    FT_Done_FreeType(m_ftLibrary);
    m_ftLibrary = nullptr;
  }
  m_backend = nullptr;
  m_textureManager = nullptr;
}

CairoGlyphRenderer::TextMetrics
CairoGlyphRenderer::measureGlyph(float contentScale, char32_t codepoint, float fontSize) {
  if (m_face == nullptr || codepoint == 0 || fontSize <= 0.0F) {
    return {};
  }

  const CacheKey key{.codepoint = codepoint, .sizeQ = quantizeSize(fontSize), .scaleQ = quantizeScale(contentScale)};
  if (const auto it = m_metrics.find(key); it != m_metrics.end()) {
    return it->second;
  }
  const float rasterSize = std::max(1.0F, fontSize * contentScale);
  const float invScale = 1.0F / contentScale;

  const FT_UInt glyphIndex = FT_Get_Char_Index(m_face, codepoint);
  if (glyphIndex == 0) {
    return {};
  }

  cairo_scaled_font_t* scaledFont = create_scaled_font(m_cairoFace, m_fontOptions, rasterSize);
  if (scaledFont == nullptr || cairo_scaled_font_status(scaledFont) != CAIRO_STATUS_SUCCESS) {
    if (scaledFont != nullptr) {
      cairo_scaled_font_destroy(scaledFont);
    }
    return {};
  }

  cairo_glyph_t glyph;
  glyph.index = glyphIndex;
  glyph.x = 0.0;
  glyph.y = 0.0;

  cairo_text_extents_t extents;
  cairo_scaled_font_glyph_extents(scaledFont, &glyph, 1, &extents);
  const TextMetrics out = metrics_from_extents(extents, invScale);
  cairo_scaled_font_destroy(scaledFont);
  if (m_metrics.size() >= kMaxMetrics) {
    m_metrics.clear();
  }
  m_metrics.emplace(key, out);
  return out;
}

CairoGlyphRenderer::CacheEntry*
CairoGlyphRenderer::lookupOrRasterize(float contentScale, char32_t codepoint, float fontSize, int blurPx) {
  CacheKey key;
  key.codepoint = codepoint;
  key.sizeQ = quantizeSize(fontSize);
  key.scaleQ = quantizeScale(contentScale);
  key.blurPx = static_cast<std::uint16_t>(blurPx);

  if (CacheEntry* hit = m_cache.find(key)) {
    return hit;
  }

  const float rasterSize = std::max(1.0F, fontSize * contentScale);
  FT_Set_Pixel_Sizes(m_face, 0, static_cast<FT_UInt>(std::round(rasterSize)));

  const FT_UInt glyphIndex = FT_Get_Char_Index(m_face, codepoint);
  if (glyphIndex == 0) {
    return nullptr;
  }

  cairo_scaled_font_t* scaledFont = create_scaled_font(m_cairoFace, m_fontOptions, rasterSize);
  if (scaledFont == nullptr || cairo_scaled_font_status(scaledFont) != CAIRO_STATUS_SUCCESS) {
    if (scaledFont != nullptr) {
      cairo_scaled_font_destroy(scaledFont);
    }
    return nullptr;
  }

  cairo_glyph_t glyph;
  glyph.index = glyphIndex;
  glyph.x = 0.0;
  glyph.y = 0.0;

  cairo_text_extents_t extents;
  cairo_scaled_font_glyph_extents(scaledFont, &glyph, 1, &extents);

  // Surface size: ceil the ink rect and add a 1px margin to protect antialiased
  // edges from clipping, plus the blur's reach.
  const int pad = 1 + blurReach(blurPx);
  const int pxWidth = std::max(1, static_cast<int>(std::ceil(extents.width)) + pad * 2);
  const int pxHeight = std::max(1, static_cast<int>(std::ceil(extents.height)) + pad * 2);

  // Position the glyph so its ink rect lands within the surface with `pad` inset.
  glyph.x = -extents.x_bearing + pad;
  glyph.y = -extents.y_bearing + pad;

  CacheEntry entry{};
  entry.pixelWidth = pxWidth;
  entry.pixelHeight = pxHeight;
  entry.baselineXPx = static_cast<float>(glyph.x);
  // Baseline from top of surface = glyph.y (since cairo baseline = y offset
  // of origin, and we placed origin so that ink-top = pad).
  entry.baselinePx = static_cast<float>(glyph.y);
  entry.inkOffsetXPx = static_cast<float>(pad);
  entry.inkOffsetYPx = static_cast<float>(pad);

  // A8 (alpha coverage) rasterization: color gets applied in the shader via
  // u_tint, so the cache is color-independent. cairo draws coverage by setting
  // an opaque source on the A8 surface (rgb is ignored for A8).
  cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_A8, pxWidth, pxHeight);
  cairo_t* cr = cairo_create(surface);
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  cairo_set_scaled_font(cr, scaledFont);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
  cairo_show_glyphs(cr, &glyph, 1);
  cairo_destroy(cr);
  cairo_surface_flush(surface);

  const int stride = cairo_image_surface_get_stride(surface);
  unsigned char* data = cairo_image_surface_get_data(surface);

  // Repack tightly to pxWidth bytes per row (cairo's stride is 4-byte aligned).
  std::vector<unsigned char> tight(static_cast<std::size_t>(pxWidth) * static_cast<std::size_t>(pxHeight));
  for (int y = 0; y < pxHeight; ++y) {
    const auto row = static_cast<std::size_t>(y);
    std::memcpy(
        tight.data() + row * static_cast<std::size_t>(pxWidth), data + row * static_cast<std::size_t>(stride),
        static_cast<std::size_t>(pxWidth)
    );
  }
  cairo_surface_destroy(surface);
  cairo_scaled_font_destroy(scaledFont);
  blurAlpha(tight, pxWidth, pxHeight, blurPx);

  if (m_textureManager == nullptr) {
    return nullptr;
  }

  entry.texture = m_textureManager->loadFromPixels(
      tight.data(), pxWidth, pxHeight, TextureDataFormat::Alpha, TextureFilter::Linear
  );
  if (entry.texture.id == 0) {
    return nullptr;
  }
  entry.bytes = static_cast<std::size_t>(pxWidth) * static_cast<std::size_t>(pxHeight);

  const float invScale = 1.0F / contentScale;
  entry.metrics = metrics_from_extents(extents, invScale);

  return &m_cache.put(key, entry);
}

void CairoGlyphRenderer::evictUnusedBefore(std::chrono::steady_clock::time_point cutoff) { m_cache.evictUnusedBefore(cutoff); }

void CairoGlyphRenderer::drawGlyph(
    float contentScale, float surfaceWidth, float surfaceHeight, float x, float baselineY, char32_t codepoint,
    float fontSize, const Color& color, const Mat3& transform, float blur
) {
  if (m_face == nullptr || m_backend == nullptr || codepoint == 0) {
    return;
  }

  // A glyph scaled up by its node (dock magnify) rasters at that size, in 0.25 steps up, so it stays sharp and an
  // animated scale re-rasters at most a few times.
  const float nodeScale = std::sqrt(std::abs(transform.m[0] * transform.m[4] - transform.m[1] * transform.m[3]));
  const float rasterScale = contentScale * std::max(1.0F, std::ceil(nodeScale * 4.0F - 0.01F) / 4.0F);
  const int blurPx = blur > 0.0F ? std::max(1, static_cast<int>(std::lround(blur * rasterScale))) : 0;
  CacheEntry* entry = lookupOrRasterize(rasterScale, codepoint, fontSize, blurPx);
  if (entry == nullptr || entry->texture.id == 0) {
    return;
  }

  const float invScale = 1.0F / rasterScale;
  const float quadW = static_cast<float>(entry->pixelWidth) * invScale;
  const float quadH = static_cast<float>(entry->pixelHeight) * invScale;
  const float baselineXLocal = entry->baselineXPx * invScale;
  const float baselineLocal = entry->baselinePx * invScale;

  const Mat3 localTranslation = Mat3::translation(x - baselineXLocal, baselineY - baselineLocal);
  Mat3 world = transform * localTranslation;

  // Snap the visible ink origin to the nearest buffer pixel so linear filtering
  // samples land on texel centers without the 1px texture pad biasing icon alignment.
  // Skip when the transform has rotation/skew — snapping then introduces
  // whole-pixel jumps per frame and makes animations look jittery.
  if (isAxisAligned(world)) {
    world.m[6] = snapToBufferPixel(world.m[6], contentScale);
    world.m[7] = snapToBufferPixel(world.m[7], contentScale);
  }

  m_backend->drawGlyph(
      RenderGlyphDraw{
          .texture = entry->texture.id,
          .surfaceWidth = surfaceWidth,
          .surfaceHeight = surfaceHeight,
          .width = quadW,
          .height = quadH,
          .opacity = 1.0F,
          .tint = color,
          .tinted = true,
          .transform = world,
      }
  );
}
