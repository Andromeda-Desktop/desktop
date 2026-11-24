#pragma once

#include "core/timer_manager.h"
#include "design_tokens.h"
#include "render/backend/render_backend.h"
#include "render/core/damage_region.h"
#include "render/core/renderer.h"
#include "render/text/cairo_glyph_renderer.h"
#include "render/text/cairo_text_renderer.h"
#include "ui/signal.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>

class GlSharedContext;
class Node;
class RenderBackend;
class RenderTarget;
enum class RenderGraphicsResetStatus;
struct Mat3;
struct WallpaperMaskDrawParams;

class RenderContext {
public:
  RenderContext();
  ~RenderContext();

  RenderContext(const RenderContext&) = delete;
  RenderContext& operator=(const RenderContext&) = delete;

  // initializeText() then attachGl().
  void initialize(GlSharedContext& shared);
  // Text measuring (fontconfig, Pango, FreeType) without GL: scenes build and lay out before EGL is up.
  void initializeText();
  // GL comes later: the first GL use (or ensureGl()) runs attach, which waits for EGL and calls attachGl().
  void attachGlLater(std::function<void()> attach);
  void attachGl(GlSharedContext& shared);
  // Runs a pending attach. False when GL is missing or its attach threw before (the throw reaches the first caller).
  bool ensureGl();
  [[nodiscard]] bool glReady() const noexcept { return m_glState == GlState::Ready; }
  void cleanup();
  void prepareForGraphicsReset();
  void restoreAfterGraphicsReset(GlSharedContext& shared);
  void finishGraphicsResetRecovery() noexcept { m_graphicsResetPending = false; }

  // Repaints what changed since the last frame; false when nothing changed and nothing was presented.
  bool renderScene(RenderTarget& target, Node* sceneRoot, const WallpaperMaskDrawParams* wallpaperMask = nullptr);
  // Returns false if the surface could not be made current (e.g. teardown);
  // best-effort callers may ignore it, render paths must skip the frame.
  bool makeCurrent(RenderTarget& target);
  // See RenderBackend::readPixels.
  bool readPixels(RenderTarget& target, std::span<std::uint8_t> rgba);
  void setTextFontFamily(std::string family);
  void setTextScale(float scale);
  void setTextBaseDirection(bool rtl);
  void notifyFontConfigChanged();

  // Request that uploaded text- and icon-glyph textures be dropped and
  // re-rasterized. The drop is deferred to the next renderScene so it runs with
  // the context current. Used to recover from GPU memory loss across
  // suspend/resume.
  void invalidateGlyphTexturesNextFrame() noexcept { m_glyphTexturesDirty = true; }
  void invalidateGpuResourcesNextFrame() noexcept;

  [[nodiscard]] RenderBackend& backend();
  [[nodiscard]] const RenderBackend& backend() const noexcept { return *m_backend; }

  // Texture/text-generation access for ScaledRenderer views and graphics-lifecycle owners.
  [[nodiscard]] TextureManager& textureManager();
  [[nodiscard]] std::uint64_t textMetricsGeneration() const noexcept { return m_textMetricsGeneration; }
  [[nodiscard]] float textScale() const noexcept { return m_textScale; }

  // Frees every unused GPU cache entry: text and glyph textures here, then each cache connected to
  // unusedCacheRelease. The one trim entry for memory pressure, hide and lock (docs/qos.md §5).
  void releaseUnusedCaches();
  // Caches outside this context that keep unused GPU entries (AsyncTextureCache) connect here.
  [[nodiscard]] Signal<>& unusedCacheRelease() noexcept { return m_unusedCacheRelease; }
  // The cache every Image load on this context decodes through; it must outlive its use here.
  void setAsyncTextures(AsyncTextureCache* cache) noexcept { m_asyncTextures = cache; }
  [[nodiscard]] AsyncTextureCache* asyncTextures() const noexcept { return m_asyncTextures; }

private:
  // Text setters free glyph textures only once GL is attached; before that none exist, and a pending attach waits.
  void makeCurrentIfGlReady();
  friend class ScaledRenderer;

  // Scale-parameterized measurement/text ops. ScaledRenderer forwards here with
  // an explicit scale; no call mutates any shared render scale.
  void setTextCaching(bool enabled) noexcept { m_textRenderer.setCaching(enabled); }
  [[nodiscard]] TextMetrics measureTextScaled(
      float scale, std::string_view text, float fontSize, const TextFont& font, float maxWidth, int maxLines,
      TextAlign align, TextEllipsize ellipsize, bool useMarkup
  );
  [[nodiscard]] TextMetrics measureFontScaled(float scale, float fontSize, const TextFont& font);
  [[nodiscard]] TextAlphaRaster
  rasterizeTextAlphaScaled(float scale, std::string_view text, float fontSize, const TextFont& font) {
    return m_textRenderer.rasterizeAlpha(scale, text, fontSize, font);
  }
  void measureTextCursorStopsScaled(
      float scale, std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets,
      std::vector<float>& outStops, const TextFont& font
  );
  void measureTextCursorStopsWrappedScaled(
      float scale, std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, float maxWidth,
      std::vector<TextCursorStop>& outStops, const TextFont& font
  );
  void measureTextCharSpansScaled(
      float scale, std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets,
      std::vector<TextCharSpan>& outSpans, const TextFont& font
  );
  [[nodiscard]] TextMetrics measureGlyphScaled(float scale, char32_t codepoint, float fontSize);

  bool makeCurrentNoSurface();
  void handleGraphicsReset(RenderGraphicsResetStatus status);
  void findOccluders(const Node* node, const Mat3& parentTransform, float parentOpacity, DamageRect clip,
                     std::uint32_t blockedPasses);
  void renderNode(
      float renderScale, const Node* node, const Mat3& parentTransform, float parentOpacity, float sw, float sh,
      float bw, float bh, float clipLeft, float clipTop, float clipRight, float clipBottom, bool hasClip,
      bool ignoreNodeOpacity, bool parentPaintContained
  );
  // gridPhases: the TerminalGridPhase bits of a TerminalGrid node; other nodes ignore it.
  void drawNodeSelf(
      float renderScale, const Node* node, const Mat3& worldTransform, float effectiveOpacity, float sw, float sh,
      float bw, float bh, std::uint8_t gridPhases = kTerminalGridAll
  );

  enum class GlState : std::uint8_t { None, Pending, Ready, Failed };
  GlState m_glState = GlState::None;
  std::function<void()> m_glAttach; // set only while Pending
  std::unique_ptr<RenderBackend> m_backend;
  CairoTextRenderer m_textRenderer;
  CairoGlyphRenderer m_glyphRenderer;
  Signal<> m_unusedCacheRelease;
  AsyncTextureCache* m_asyncTextures = nullptr;
  std::string m_textFontFamily{Tokens::fontFamily}; // the design font; the shell overrides it from its config
  bool m_textBaseDirRtl = false;
  std::uint64_t m_textMetricsGeneration = 1;
  float m_textScale = 1.0F;
  std::uint64_t m_gpuResourceGeneration = 0;
  bool m_glyphTexturesDirty = false;
  // ANDROMEDA_PROFILE frame stats: nodes drawn and culled in the current renderScene.
  std::uint32_t m_statDrawn = 0;
  std::uint32_t m_statCulled = 0;
  // Frees text and icon textures of a shell that stopped drawing (step 5 runs only inside frames).
  void expireIdleCaches();
  Timer m_cacheExpiryTimer;
  std::chrono::steady_clock::time_point m_lastSceneDraw;
  // Repaint rects of a damage frame, drawn in one scene walk. They do not overlap in buffer pixels, so a node drawn
  // once per rect blends each pixel once. Empty in a full repaint.
  struct RepaintPass {
    RenderScissor scissor;
    DamageRect logical;
    const Node* occluder = nullptr; // topmost node that covers the pass with opaque pixels; draws before it are hidden
    bool occluderReached = false;
  };
  std::array<RepaintPass, DamageRegion::kMaxRects> m_passes{};
  std::size_t m_passCount = 0;
  int m_roundedClipDepth = 0; // open pushRoundedClip calls in the current renderNode walk
  bool m_graphicsResetPending = false;
  GlSharedContext* m_shared = nullptr;
};
