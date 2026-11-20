#pragma once

#include "render/core/renderer.h"
#include "render/core/texture_handle.h"
#include "render/text/text_glyph_atlas.h"

#include <atomic>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Forward declarations to avoid dragging Pango headers into every TU.
using PangoContext = struct _PangoContext;
using PangoFontDescription = struct _PangoFontDescription;
using PangoFontMap = struct _PangoFontMap;
using PangoLayout = struct _PangoLayout;

class RenderBackend;
class TextureManager;
struct Color;
struct Mat3;

// Pango/Cairo-backed text renderer.
//
// Rasterizes a shaped PangoLayout into an ARGB32 Cairo surface, uploads it as
// a premultiplied RGBA texture, and submits glyph quads to RenderBackend.
// Handles Latin, CJK, Arabic, BiDi, and COLR v1 emoji via fontconfig fallback.
//
// measure() and truncate() do not require a current render context.
// draw() needs the render backend context current (creates/binds textures).
class CairoTextRenderer {
public:
  struct TextMetrics {
    float width = 0.0F;
    float left = 0.0F;
    float right = 0.0F;
    float top = 0.0F;       // negative — above baseline
    float bottom = 0.0F;    // positive — below baseline
    float inkTop = 0.0F;    // negative — visible ink above baseline
    float inkBottom = 0.0F; // positive — visible ink below baseline
    float inkLeft = 0.0F;   // visible ink left edge relative to layout origin
    float inkRight = 0.0F;  // visible ink right edge relative to layout origin
    float capHeight = 0.0F; // measured baseline-to-cap-top of 'H' (0 if unavailable)
    int lineCount = 0;      // laid-out line count (0 for empty text)
    // Left edge of the leftmost line within maxWidth; rasterizeLayout crops it.
    float alignOffsetX = 0.0F;
  };

  CairoTextRenderer();
  ~CairoTextRenderer();

  CairoTextRenderer(const CairoTextRenderer&) = delete;
  CairoTextRenderer& operator=(const CairoTextRenderer&) = delete;

  void initialize(RenderBackend* backend, TextureManager* textures);
  // Measuring needs only initialize(); drawing needs the backend too.
  void attachBackend(RenderBackend* backend, TextureManager* textures) noexcept {
    m_backend = backend;
    m_textureManager = textures;
  }
  void cleanup();

  // App zygote, before fork on its only thread: loads the fonts listed in `file` (one Pango font description per
  // line) into a font map that the next initialize() in this process adopts.
  static void preloadFonts(const std::filesystem::path& file);
  // Records the first fonts this process loads; cleanup() writes them to `file` for preloadFonts().
  static void recordFontsTo(std::filesystem::path file);

  void setFontFamily(std::string family);
  // Multiplies every text size (accessibility text_scale); symbols drawn by the glyph renderer keep their size.
  void setTextScale(float scale);
  void setBaseDirection(bool rtl);
  void notifyFontConfigChanged();

  // Drops the uploaded glyph textures (keeping CPU-side metrics) so they are
  // re-rasterized on the next draw. Used to recover from GPU memory loss across
  // suspend/resume on drivers that do not preserve VRAM. Requires the render
  // context to be current.
  void invalidateGlyphTextures();
  // Frees entries last drawn before `cutoff`; keeps the most recent one. Requires the render context to be current.
  void evictUnusedBefore(std::chrono::steady_clock::time_point cutoff);
  void abandonGlyphTextures() noexcept;
  // Off while sensitive text (a revealed password) is measured or drawn: no cache keeps it. Its rasters live until
  // releaseTransient(), which the owner calls once the GPU is done with the last frame.
  void setCaching(bool enabled) noexcept { m_caching = enabled; }
  void releaseTransient();
  // Every renderer drops its caches at its next releaseTransient(): text drawn unmasked (a password field masked
  // again) must not stay cached.
  static void forgetAllText() noexcept { s_textEpoch.fetch_add(1, std::memory_order_relaxed); }
  // True when a cache keeps `text` (raster or metrics).
  [[nodiscard]] bool holdsText(std::string_view text) const;

  [[nodiscard]] TextMetrics measure(
      float contentScale, std::string_view text, float fontSize, const TextFont& font = {}, float maxWidth = 0.0F,
      int maxLines = 0, TextAlign align = TextAlign::Start, TextEllipsize ellipsize = TextEllipsize::End,
      bool useMarkup = false
  );
  // Metrics of `font` (an empty family = the active family set by setFontFamily()).
  [[nodiscard]] TextMetrics measureFont(float contentScale, float fontSize, const TextFont& font = {}) const;
  void measureCursorStops(
      float contentScale, std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets,
      std::vector<float>& outStops, const TextFont& font = {}
  );
  void measureCursorStopsWrapped(
      float contentScale, std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets,
      float maxWidth, std::vector<TextCursorStop>& outStops, const TextFont& font = {}
  );
  void measureCharSpans(
      float contentScale, std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets,
      std::vector<TextCharSpan>& outSpans, const TextFont& font = {}
  );

  // One line of `text` as 8-bit coverage, shaped as draw() shapes it (see TextAlphaRaster).
  [[nodiscard]] TextAlphaRaster
  rasterizeAlpha(float contentScale, std::string_view text, float fontSize, const TextFont& font);

  void draw(
      float contentScale, float surfaceWidth, float surfaceHeight, float x, float baselineY, std::string_view text,
      float fontSize, const Color& color, const Mat3& transform, const TextFont& font = {}, float maxWidth = 0.0F,
      int maxLines = 0, TextAlign align = TextAlign::Start, TextEllipsize ellipsize = TextEllipsize::End,
      bool useMarkup = false
  );

private:
  struct CacheKey {
    std::string text;
    TextFont font;
    std::uint32_t sizeBits = 0;     // exact normalized fontSize bits
    std::uint32_t colorRgba = 0;    // packed r<<24|g<<16|b<<8|a
    std::uint32_t maxWidthBits = 0; // exact normalized maxWidth bits; 0 = no limit
    std::uint32_t scaleBits = 0;    // exact normalized contentScale bits
    std::uint16_t maxLines = 0;     // 0 = no explicit limit (use '\n'-count fallback)
    TextAlign align = TextAlign::Start;
    TextEllipsize ellipsize = TextEllipsize::End;
    bool useMarkup = false;

    bool operator==(const CacheKey& other) const noexcept;
  };
  struct CacheKeyHash {
    std::size_t operator()(const CacheKey& k) const noexcept;
  };

  // Color-independent key used to cache logical TextMetrics. measure() is
  // called from layout paths that run every frame on dirty surfaces; rebuilding
  // a PangoLayout for each call was the top allocation hot-spot in heaptrack.
  struct MetricsKey {
    std::string text;
    TextFont font;
    std::uint32_t sizeBits = 0;
    std::uint32_t maxWidthBits = 0;
    std::uint32_t scaleBits = 0;
    std::uint16_t maxLines = 0;
    TextAlign align = TextAlign::Start;
    TextEllipsize ellipsize = TextEllipsize::End;
    bool useMarkup = false;

    bool operator==(const MetricsKey& other) const noexcept;
  };
  struct MetricsKeyHash {
    std::size_t operator()(const MetricsKey& k) const noexcept;
  };

  // Color- and text-independent key for measureFont(), which derives font
  // metrics from a font at a given size/scale. measureFont()
  // runs every frame from bar layout; each underlying pango_context_get_metrics
  // call makes Pango accumulate internal cache structures that are never
  // reclaimed during the run (heaptrack: top leak, ~24MB over a 30m session).
  // Memoizing collapses ~15k calls/run down to the handful of distinct fonts.
  // An empty family means the active family; clearCaches() runs when it changes.
  struct FontMetricsKey {
    TextFont font;
    std::uint32_t sizeBits = 0;
    std::uint32_t scaleBits = 0;
    bool operator==(const FontMetricsKey& other) const noexcept;
  };
  struct FontMetricsKeyHash {
    std::size_t operator()(const FontMetricsKey& k) const noexcept;
  };

  // LruList is a list of pointers into map keys — we break the otherwise
  // circular type dependency (CacheEntry ↔ CacheMap) this way. CacheKey* is
  // stable because unordered_map never moves key-value nodes under insert
  // (we also reserve bucket capacity upfront to avoid rehashing).
  using LruList = std::list<const CacheKey*>;

  // A very tall text block can exceed the backend texture limit (typically
  // 4096 or 8192). We slice the Pango layout into N vertically-stacked tiles,
  // each its own texture sized within that limit. draw() emits one quad per tile;
  // tiles abut on exact buffer-pixel boundaries so there is no visible seam.
  struct Tile {
    TextureHandle texture;
    int pixelHeight = 0;  // raster pixels
    int pixelYOffset = 0; // from top of full layout, in raster pixels
  };

  struct CacheEntry {
    std::vector<Tile> tiles;
    int pixelWidth = 0;   // total raster surface pixel width
    int pixelHeight = 0;  // total raster surface pixel height (sum of tiles)
    float baselinePx = 0; // baseline from top of full layout, in raster pixels
    float inkOffsetX = 0; // raster px from surface left to logical text origin
    TextMetrics metrics;  // logical metrics in logical (unscaled) pixels
    std::size_t bytes = 0;
    // Rgba: premul RGBA tiles. Coverage: alpha tiles, tint in shader. Atlas: no tiles, quads into m_atlas.
    enum class Ink : std::uint8_t { Rgba, Coverage, Atlas } ink = Ink::Rgba;
    std::vector<float> quadPositions; // Atlas: x, y per vertex, raster px from the surface top-left
    std::vector<float> quadTexcoords;
    std::uint64_t atlasGeneration = 0; // m_atlas.generation() the quads were built against
    LruList::iterator lruIt;
    std::chrono::steady_clock::time_point lastUsed;
  };

  using CacheMap = std::unordered_map<CacheKey, CacheEntry, CacheKeyHash>;
  using MetricsMap = std::unordered_map<MetricsKey, TextMetrics, MetricsKeyHash>;
  using ShapedMap = std::unordered_map<MetricsKey, PangoLayout*, MetricsKeyHash>;
  using FontMetricsMap = std::unordered_map<FontMetricsKey, TextMetrics, FontMetricsKeyHash>;

  // Font description for `font` at a raster pixel size; an empty family resolves to
  // the active family. Caller owns it (pango_font_description_free).
  PangoFontDescription* newFontDescription(const TextFont& font, float rasterSize, float opsz) const;
  // Build a PangoLayout at the given scaled size. Caller owns the layout (g_object_unref).
  PangoLayout* buildLayout(
      float contentScale, std::string_view text, float fontSize, const TextFont& font, float maxWidthPxScaled,
      int maxLines, TextAlign align, TextEllipsize ellipsize = TextEllipsize::End, bool useMarkup = false
  ) const;
  // Render a layout into a new GL texture; fills out fields of `entry`.
  // When `tinted` is true, rasterizes as CAIRO_FORMAT_A8 and uploads alpha
  // coverage so the color is applied via u_tint at draw time. When false,
  // rasterizes as CAIRO_FORMAT_ARGB32 with `color` baked in (for COLR emoji
  // content).
  void rasterizeLayout(
      float contentScale, PangoLayout* layout, const Color& color, bool tinted, float strokePx, const TextFont& font,
      CacheEntry& entry, bool atlas = false
  );
  // Fills entry with atlas quads; false when a glyph cannot come from the atlas (no Cairo font, unknown glyph).
  bool buildAtlasQuads(PangoLayout* layout, int originXPx, int extraTopPx, CacheEntry& entry);
  // Extract logical metrics from a laid-out PangoLayout, dividing by PANGO_SCALE and by scale.
  TextMetrics metricsFromLayout(float contentScale, PangoLayout* layout) const;

  CacheEntry* lookupOrRasterize(
      float contentScale, std::string_view text, float fontSize, const TextFont& font, float maxWidth, int maxLines,
      TextAlign align, const Color& color, TextEllipsize ellipsize = TextEllipsize::End, bool useMarkup = false
  );
  void touch(CacheMap::iterator it);
  void evict(CacheMap::iterator it);
  void evictIfNeeded();
  // The layout measure() shaped for `key`, now owned by the caller; nullptr when none is kept.
  PangoLayout* takeShaped(const MetricsKey& key);
  void dropShaped(ShapedMap& map);
  void clearCaches();
  // Re-read fontconfig when a font has been registered (by any surface) since the
  // last measure/draw, so newly loaded plugin fonts become resolvable here.
  void maybeSyncFontConfig();

  bool m_fontConfigInitialized = false;
  mutable std::vector<std::string> m_recordedFonts;
  std::uint64_t m_syncedFontGeneration = 0;
  std::string m_fontFamily = "sans-serif";
  bool m_baseDirRtl = false;

  PangoFontMap* m_fontMap = nullptr;      // owned
  PangoContext* m_pangoContext = nullptr; // owned
  RenderBackend* m_backend = nullptr;
  TextureManager* m_textureManager = nullptr;

  CacheMap m_cache;
  LruList m_lru;
  std::size_t m_cacheBytes = 0;
  int m_glMaxTextureSize = 0; // lazy-queried on first rasterize

  MetricsMap m_metricsCache;
  mutable FontMetricsMap m_fontMetricsCache;
  float m_textScale = 1.0F;
  bool m_caching = true;
  std::list<CacheEntry> m_transient;
  TextGlyphAtlas m_atlas;
  std::vector<float> m_runPositions; // draw() scratch: atlas quads in surface pixels
  // Layouts measure() shaped, so a draw miss of the same key takes one instead of shaping again. Owned. Each
  // releaseTransient() (frame start) drops m_shapedOld and moves m_shaped there: a layout lives about one frame.
  ShapedMap m_shaped;
  ShapedMap m_shapedOld;
  std::uint64_t m_textEpoch = 0; // the s_textEpoch this renderer's caches were last cleared for
  inline static std::atomic<std::uint64_t> s_textEpoch{0};

  static constexpr std::size_t kMaxCacheEntries = 512;
  static constexpr std::size_t kMaxCacheBytes = 32 * 1024 * 1024;
  static constexpr std::size_t kMaxMetricsEntries = 1024;
  static constexpr std::size_t kMaxShapedLayouts = 256;
  static constexpr std::size_t kMaxFontMetricsEntries = 64;
};
