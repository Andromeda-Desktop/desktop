#pragma once

#include "render/core/texture_handle.h"
#include "util/bounded_cache.h"

#include <chrono>
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>

// FreeType forwards
using FT_Library = struct FT_LibraryRec_*;
using FT_Face = struct FT_FaceRec_*;

// Cairo forwards
using cairo_font_face_t = struct _cairo_font_face;
using cairo_font_options_t = struct _cairo_font_options;

class RenderBackend;
class TextureManager;
struct Color;
struct Mat3;

// Direct FreeType + Cairo renderer for single codepoints in a dedicated font
// (noctalia-tabler.ttf icon font). No Pango, no shaping, no fallback — the icon font
// MUST be used, never substituted.
class CairoGlyphRenderer {
public:
  struct TextMetrics {
    float width = 0.0F;
    float left = 0.0F;
    float right = 0.0F;
    float top = 0.0F;    // negative — above baseline
    float bottom = 0.0F; // positive — below baseline
  };

  CairoGlyphRenderer();
  ~CairoGlyphRenderer();

  CairoGlyphRenderer(const CairoGlyphRenderer&) = delete;
  CairoGlyphRenderer& operator=(const CairoGlyphRenderer&) = delete;

  void initialize(const std::string& fontPath, RenderBackend* backend, TextureManager* textures);
  void attachBackend(RenderBackend* backend, TextureManager* textures) noexcept {
    m_backend = backend;
    m_textureManager = textures;
  }
  void cleanup();

  // Drops the uploaded icon-glyph textures so they are re-rasterized on the next
  // draw. Recovers from GPU memory loss across suspend/resume. Requires the
  // render context to be current.
  void invalidateGlyphTextures();
  // Frees entries last drawn before `cutoff`; keeps the most recent one. Requires the render context to be current.
  void evictUnusedBefore(std::chrono::steady_clock::time_point cutoff);
  void abandonGlyphTextures() noexcept;

  [[nodiscard]] TextMetrics measureGlyph(float contentScale, char32_t codepoint, float fontSize);

  // `blur` > 0 draws the glyph's coverage blurred by that radius (logical px), as a soft shadow.
  void drawGlyph(
      float contentScale, float surfaceWidth, float surfaceHeight, float x, float baselineY, char32_t codepoint,
      float fontSize, const Color& color, const Mat3& transform, float blur = 0.0F
  );

private:
  struct CacheKey {
    char32_t codepoint = 0;
    std::uint32_t sizeQ = 0;
    std::uint16_t scaleQ = 0;
    std::uint16_t blurPx = 0; // blur radius in raster px; 0 = sharp

    bool operator==(const CacheKey& other) const noexcept;
  };
  struct CacheKeyHash {
    std::size_t operator()(const CacheKey& k) const noexcept;
  };

  struct CacheEntry {
    TextureHandle texture;
    int pixelWidth = 0;
    int pixelHeight = 0;
    float baselineXPx = 0.0F; // baseline from left of surface, raster pixels
    float baselinePx = 0.0F;  // baseline from top of surface, raster pixels
    float inkOffsetXPx = 0.0F;
    float inkOffsetYPx = 0.0F;
    TextMetrics metrics;
    std::size_t bytes = 0;
  };

  CacheEntry* lookupOrRasterize(float contentScale, char32_t codepoint, float fontSize, int blurPx);

  FT_Library m_ftLibrary = nullptr;
  FT_Face m_face = nullptr;
  cairo_font_face_t* m_cairoFace = nullptr;
  cairo_font_options_t* m_fontOptions = nullptr;
  RenderBackend* m_backend = nullptr;
  TextureManager* m_textureManager = nullptr;

  BoundedCache<CacheKey, CacheEntry, CacheKeyHash> m_cache;
  // Metrics depend only on the face: they outlive the textures. Key blurPx is always 0.
  std::unordered_map<CacheKey, TextMetrics, CacheKeyHash> m_metrics;
};
