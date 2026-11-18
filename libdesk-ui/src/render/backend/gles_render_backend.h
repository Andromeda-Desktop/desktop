#pragma once

#include "render/backend/gles_texture_manager.h"
#include "render/backend/render_backend.h"
#include "render/core/shader_program.h"
#include "render/programs/audio_spectrum_program.h"
#include "render/programs/blur_program.h"
#include "render/programs/countdown_ring_program.h"
#include "render/programs/effect_program.h"
#include "render/programs/fancy_audio_visualizer_program.h"
#include "render/programs/glyph_program.h"
#include "render/programs/graph_program.h"
#include "render/programs/terminal_grid_program.h"
#include "render/programs/shader_scene_program.h"
#include "render/programs/image_program.h"
#include "render/programs/lockscreen_transition_program.h"
#include "render/programs/rect_program.h"
#include "render/programs/screen_corner_program.h"
#include "render/programs/spinner_program.h"
#include "render/programs/wallpaper_mask_program.h"
#include "render/programs/wallpaper_program.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

struct GpuSplitTimer;

class GlesRenderBackend final : public RenderBackend {
public:
  GlesRenderBackend();
  ~GlesRenderBackend() override;

  GlesRenderBackend(const GlesRenderBackend&) = delete;
  GlesRenderBackend& operator=(const GlesRenderBackend&) = delete;

  void initialize(GlSharedContext& shared) override;
  void cleanup() override;

  bool makeCurrent(RenderTarget& target) override;
  bool makeCurrentNoSurface() override;
  bool beginFrame(RenderTarget& target) override;
  [[nodiscard]] int bufferAge(RenderTarget& target) override;
  void endFrame(RenderTarget& target, std::span<const DamageRect> damage = {}) override;
  [[nodiscard]] RenderGraphicsResetStatus graphicsResetStatus() override;
  void invalidateGpuResources() override;
  void abandonAfterGraphicsReset() noexcept override;

  [[nodiscard]] std::unique_ptr<RenderSurfaceTarget> createSurfaceTarget(std::unique_ptr<NativeEglWindow> window) override;
  [[nodiscard]] std::unique_ptr<RenderSurfaceTarget> createOffscreenTarget() override;
  bool readPixels(RenderTarget& target, std::span<std::uint8_t> rgba) override;
  [[nodiscard]] std::unique_ptr<RenderFramebuffer>
  createFramebuffer(std::uint32_t width, std::uint32_t height) override;
  void bindFramebuffer(const RenderFramebuffer& framebuffer) override;
  void bindDefaultFramebuffer() override;
  void setViewport(std::uint32_t width, std::uint32_t height) override;
  void clear(Color color) override;
  void setBlendMode(RenderBlendMode mode) override;
  [[nodiscard]] int maxTextureSize() override;
  void setScissor(RenderScissor scissor) override;
  void disableScissor() override;
  void pushRoundedClip(
      float surfaceWidth, float surfaceHeight, float width, float height, const Radii& radii, const Mat3& transform,
      RenderScissor area, int depth
  ) override;
  void popRoundedClip(
      float surfaceWidth, float surfaceHeight, float width, float height, const Radii& radii, const Mat3& transform,
      RenderScissor area, int depth
  ) override;
  void drawRect(
      float surfaceWidth, float surfaceHeight, float width, float height, const RoundedRectStyle& style,
      const Mat3& transform
  ) override;
  void drawImage(const RenderImageDraw& draw) override;
  void drawGlyph(const RenderGlyphDraw& draw) override;
  void drawGlyphRun(const RenderGlyphRun& run) override;
  void flushBatches() override;
  void drawSpinner(
      float surfaceWidth, float surfaceHeight, float width, float height, const SpinnerStyle& style,
      const Mat3& transform
  ) override;
  void drawCountdownRing(
      float surfaceWidth, float surfaceHeight, float width, float height, const CountdownRingStyle& style,
      const Mat3& transform
  ) override;
  void drawScreenCorner(
      float surfaceWidth, float surfaceHeight, float width, float height, const ScreenCornerStyle& style,
      const Mat3& transform
  ) override;
  void drawAudioSpectrum(
      float surfaceWidth, float surfaceHeight, float pixelScaleX, float pixelScaleY, float width, float height,
      const AudioSpectrumStyle& style, std::span<const float> values, const Mat3& transform
  ) override;
  void drawFancyAudioVisualizer(
      TextureId audioTexture, int textureWidth, float surfaceWidth, float surfaceHeight, float width, float height,
      const FancyAudioVisualizerStyle& style, const Mat3& transform
  ) override;
  void drawEffect(
      float surfaceWidth, float surfaceHeight, float width, float height, const EffectStyle& style,
      const Mat3& transform
  ) override;
  void drawGraph(
      TextureId dataTexture, int textureWidth, float surfaceWidth, float surfaceHeight, float width, float height,
      const GraphStyle& style, const Mat3& transform
  ) override;
  void drawTerminalGrid(const TerminalGridDraw& grid) override;
  void drawShaderScene(const ShaderSceneDraw& scene) override;
  void drawWallpaper(const WallpaperDrawParams& params) override;
  void drawWallpaperMask(const WallpaperMaskDrawParams& params) override;
  void drawLockscreenTransition(const LockscreenTransitionDrawParams& params) override;
  void drawFullscreenTexture(TextureId texture, bool flipY) override;
  void drawFullscreenTint(Color color) override;
  void drawFramebufferBlur(
      TextureId sourceTexture, std::uint32_t width, std::uint32_t height, float directionX, float directionY,
      float radius
  ) override;

  [[nodiscard]] TextureManager& textureManager() override { return m_textureManager; }

private:
  // Draws the queued batchable rects; every call that draws, changes GL state or ends the frame runs it first.
  void flushRectBatch();
  void flushGlyphBatch();
  [[nodiscard]] BatchStats takeBatchStats() override { return std::exchange(m_batchStats, {}); }
  void drawFullscreenQuad(const ShaderProgram& program);
  void ensureFullscreenTextureProgram();
  void ensureFullscreenTintProgram();
  void destroyGpuObjects();
  void abandonGpuObjects() noexcept;

  EGLDisplay m_display = EGL_NO_DISPLAY;
  bool m_bufferAge = false;
  PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC m_swapWithDamage = nullptr;
  EGLConfig m_config = nullptr;
  EGLContext m_context = EGL_NO_CONTEXT;
  int m_maxTextureSize = 0;
  bool m_viewportValid = false;
  std::uint32_t m_viewportWidth = 0;
  std::uint32_t m_viewportHeight = 0;
  std::optional<RenderBlendMode> m_blendMode;
  bool m_scissorEnabled = false;
  void writeClipStencil(
      float surfaceWidth, float surfaceHeight, float width, float height, const Radii& radii, const Mat3& transform,
      RenderScissor area, unsigned op, int ref
  );
  bool m_scissorValid = false;
  RenderScissor m_scissor;
  GlesTextureManager m_textureManager;
  RectProgram m_rectProgram;
  // Vertices of queued batchable rects (RectProgram::appendBatched).
  std::vector<float> m_rectBatch;
  // Vertices of queued atlas glyph runs (GlyphProgram::appendTintedRun), all from m_glyphBatchTexture. At most one of
  // the two batches holds vertices, so draw order stays as issued.
  std::vector<float> m_glyphBatch;
  TextureId m_glyphBatchTexture;
  BatchStats m_batchStats;
  // ANDROMEDA_PROFILE only: GPU time per frame split into terminal grid phases and the rest.
  std::unique_ptr<GpuSplitTimer> m_gpuTimer;
  void gpuMark(std::uint8_t kind);
  ImageProgram m_imageProgram;
  GlyphProgram m_glyphProgram;
  SpinnerProgram m_spinnerProgram;
  CountdownRingProgram m_countdownRingProgram;
  ScreenCornerProgram m_screenCornerProgram;
  AudioSpectrumProgram m_audioSpectrumProgram;
  FancyAudioVisualizerProgram m_fancyAudioVisualizerProgram;
  EffectProgram m_effectProgram;
  GraphProgram m_graphProgram;
  TerminalGridProgram m_terminalGridProgram;
  ShaderSceneProgram m_shaderSceneProgram;
  WallpaperProgram m_wallpaperProgram;
  WallpaperMaskProgram m_wallpaperMaskProgram;
  LockscreenTransitionProgram m_lockscreenTransitionProgram;
  BlurProgram m_blurProgram;
  ShaderProgram m_fullscreenTextureProgram;
  ShaderProgram m_fullscreenTintProgram;
};
