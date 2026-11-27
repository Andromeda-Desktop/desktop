#pragma once

#include "core/timer_manager.h"
#include "protocols/glass_material.h"
#include "render/core/render_styles.h"
#include "render/core/wallpaper_types.h"
#include "render/render_target.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class AnimationManager;
class FrameStatsSink;
class NativeEglWindow;
class Node;
class RenderContext;

struct InputRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;

  bool operator==(const InputRect&) const = default;
};

// A rounded glass card in surface-local logical px. `node` picks the palette tone of the tint; `onDensity` gets the
// backdrop density (0..1) the compositor measures under the card.
struct GlassCard {
  InputRect rect;
  float radius = 0.0F;
  glass::Material material;
  const Node* node = nullptr;
  std::function<void(float)> onDensity;
};

struct SurfaceIdleProfileEntry {
  std::string label;
  std::uint64_t requestUpdate = 0;
  std::uint64_t requestUpdateOnly = 0;
  std::uint64_t requestLayout = 0;
  std::uint64_t requestRedraw = 0;
  std::uint64_t queuedFrameWork = 0;
  std::uint64_t processedFrameWork = 0;
  std::uint64_t queuedRenders = 0;
  std::uint64_t processedQueuedRenders = 0;
  std::uint64_t prepareCallbacks = 0;
  std::uint64_t frameTicks = 0;
  std::uint64_t animationTicks = 0;
  std::uint64_t updateCallbacks = 0;
  std::uint64_t renders = 0;
  double prepareMs = 0.0;
  double frameTickMs = 0.0;
  double animationMs = 0.0;
  double updateMs = 0.0;
  double renderMs = 0.0;
};

struct SurfaceIdleProfileSnapshot {
  SurfaceIdleProfileEntry total;
  std::vector<SurfaceIdleProfileEntry> surfaces;
};

// What the user can see of a toplevel window, from the compositor (dsk_surface_v1.presence). Off: the session is
// locked or every output the window is on is off. Hidden: minimized, on another workspace or a background tab.
// Occluded: opaque windows cover all of it. Layer surfaces get only Shown or Off. Other surfaces, and compositors
// without the event, stay Shown.
enum class SurfacePresence : std::uint8_t { Shown, Occluded, Hidden, Off };

// A window-system surface that draws a scene: render target, frame pacing, scale and damage. A platform subclass
// (WaylandSurface) owns the native surface and implements the protected hooks.
class Surface {
public:
  using ConfigureCallback = std::function<void(std::uint32_t width, std::uint32_t height)>;
  using PrepareFrameCallback = std::function<void(bool needsUpdate, bool needsLayout)>;
  using UpdateCallback = std::function<void()>;
  using FrameTickCallback = std::function<void(float deltaMs)>;
  // EveryRedraw: each redraw asks for a tick on the next frame callback, so a tick that redraws keeps ticking.
  // OnRequest: ticks run only after requestFrameTick(); a lone redraw then asks for no frame callback.
  enum class FrameTicks : std::uint8_t { EveryRedraw, OnRequest };
  using ScaleChangedCallback = std::function<void(float scale)>;
  using PresenceChangedCallback = std::function<void(SurfacePresence presence)>;

  Surface() = default;
  virtual ~Surface();

  Surface(const Surface&) = delete;
  Surface& operator=(const Surface&) = delete;

  virtual bool initialize() = 0;

  // A popup of this surface needs keyboard focus while open; a keyboard-inert shell layer lends it until false.
  virtual void setPopupKeyboardFocus(bool /*lend*/) {}
  // A desktop-shell layer (bar, panel, dock), not an application window or popup.
  [[nodiscard]] virtual bool isShellLayer() const noexcept { return false; }

  [[nodiscard]] bool isRunning() const noexcept;

  void setConfigureCallback(ConfigureCallback callback);
  void setPrepareFrameCallback(PrepareFrameCallback callback);
  void setUpdateCallback(UpdateCallback callback);
  void setFrameTickCallback(FrameTickCallback callback, FrameTicks ticks = FrameTicks::EveryRedraw);
  void setScaleChangedCallback(ScaleChangedCallback callback);
  // The one visibility fact for content that only matters on screen (animations, polls, previews).
  [[nodiscard]] SurfacePresence presence() const noexcept { return m_presence; }
  // Frame ticks (animations, frame-tick widgets, decorative loops) run only while the presence is Shown; content
  // renders (configure, update, redraw) do not wait for it.
  [[nodiscard]] bool ticks() const noexcept { return m_presence == SurfacePresence::Shown; }
  // Runs when presence() changes.
  void setPresenceChangedCallback(PresenceChangedCallback callback) { m_presenceChangedCallback = std::move(callback); }
  // Runs once, after the next render that swaps a new buffer.
  void callOnNextPresent(std::function<void()> callback) { m_nextPresentCallback = std::move(callback); }
  virtual void setInputRegion(const std::vector<InputRect>& rects) = 0;
  virtual void setOpaqueRegion(const std::vector<InputRect>& rects) = 0;
  virtual void setBlurRegion(const std::vector<InputRect>& rects) = 0;
  // Declares the whole surface opaque, or not, from its next commit, so the compositor can skip what lies beneath it.
  virtual void setOpaque(bool opaque) = 0;
  virtual void clearBlurRegion() = 0;
  // The compositor draws `card` as one glass shape (blur, rim, tint, density) where it can; otherwise it blurs the
  // rounded rect. Returns true when the compositor draws the material tint, so the client paints no fill.
  virtual bool setGlassCard(const GlassCard& card);
  virtual void clearGlassCard() { clearBlurRegion(); }
  // Renders at this fraction of the output resolution, for a backdrop that is soft anyway. Needs viewport scaling;
  // without it the surface renders at full resolution.
  void setResolutionScale(float scale);
  // While held, the surface commits no buffer, so it stays unmapped; releasing it draws the next frame.
  void holdRender(bool held);
  // By default the first frame waits up to 50 ms for pending content (image decodes), so the surface maps
  // complete. A surface that must show at once (the lock screen) skips the wait.
  void skipFirstFrameHold();
  void setDebugName(std::string name);
  // Window name for assistive technology; empty = derived from the debug name (layer namespace).
  void setAccessibleTitle(std::string title) { m_accessibleTitle = std::move(title); }

  // Approximates a rounded rectangle as a stack of horizontal axis-aligned strips
  // suitable for a region (which has no curve primitives). The four corner radii
  // are applied independently. `stripPx` is the strip height through the corner
  // bands; smaller is smoother but produces more rects.
  static std::vector<InputRect> tessellateRoundedRect(
      int x, int y, int w, int h, float tlRadius, float trRadius, float brRadius, float blRadius, int stripPx = 1
  );
  static std::vector<InputRect> tessellateRoundedRect(int x, int y, int w, int h, float radius, int stripPx = 1) {
    return tessellateRoundedRect(x, y, w, h, radius, radius, radius, radius, stripPx);
  }
  // Tessellates the same shape the rect shader rasterizes: per-corner Concave/Convex
  // curves around a logical body. (x, y, w, h) describes the BODY rect; the visual
  // rect is the body expanded outward by logicalInset (so callers don't need to
  // pre-compute the bounding box that hosts concave-corner bulges). Concave corners
  // extend outward into the inset margin; convex corners contract the body inward.
  static std::vector<InputRect> tessellateShape(
      int x, int y, int w, int h, const CornerShapes& corners, const RectInsets& logicalInset, const Radii& radii,
      int stripPx = 1
  );
  // Tessellates a rounded rectangle rotated by `rotationRad` around its center
  // (`centerX`, `centerY`) into axis-aligned horizontal strips. When `rotationRad`
  // is zero this reduces to a plain tessellateRoundedRect call.
  static std::vector<InputRect> tessellateRotatedRoundedRect(
      float centerX, float centerY, float width, float height, float radius, float rotationRad, int stripPx = 1
  );
  // True when any rect covers at least one pixel of a `width` x `height` surface.
  // Rects are surface-local, so they may legitimately sit partly or fully outside.
  static bool regionIntersectsBounds(const std::vector<InputRect>& rects, std::uint32_t width, std::uint32_t height);
  void requestUpdate();
  void requestUpdateOnly();
  void requestLayout();
  void requestRedraw();
  void requestFrameTick();
  void renderNow();
  /// Discards an in-flight frame callback without losing queued work
  /// or the tick intent attached to that callback.
  void discardPendingFrameCallback();
  void setAnimationManager(AnimationManager* manager) noexcept { m_animationManager = manager; }
  void setSceneRoot(Node* root);
  void setRenderContext(RenderContext* ctx);
  void setWallpaperMask(std::optional<WallpaperMaskDrawParams> mask);
  [[nodiscard]] RenderContext* renderContext() const noexcept { return m_renderContext; }
  [[nodiscard]] RenderTarget& renderTarget() noexcept { return m_renderTarget; }
  [[nodiscard]] Node* sceneRoot() const noexcept { return m_sceneRoot; }
  [[nodiscard]] const std::string& debugName() const noexcept { return m_debugName; }
  [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }
  [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }
  [[nodiscard]] std::int32_t bufferScale() const noexcept { return m_bufferScale; }
  [[nodiscard]] float effectiveBufferScale() const noexcept;
  [[nodiscard]] std::uint32_t bufferWidthFor(std::uint32_t logicalWidth) const noexcept;
  [[nodiscard]] std::uint32_t bufferHeightFor(std::uint32_t logicalHeight) const noexcept;

  [[nodiscard]] static bool hasPendingFrameWork();
  static void drainPendingFrameWork();
  [[nodiscard]] static bool hasPendingRenders();
  static void drainPendingRenders();
  [[nodiscard]] static SurfaceIdleProfileSnapshot takeIdleProfileSnapshot(bool reset);

protected:
  // The platform layer reports the compositor's value; runs the callback only on a change.
  void setPresence(SurfacePresence presence);
  // Platform hooks.
  [[nodiscard]] virtual bool hasNativeSurface() const noexcept = 0;
  [[nodiscard]] virtual std::unique_ptr<NativeEglWindow> createNativeWindow() = 0;
  [[nodiscard]] virtual bool frameCallbackPending() const noexcept = 0;
  // Asks for one frame callback with the next commit; it arrives as frameDone().
  virtual void requestFrameCallback() = 0;
  virtual void cancelFrameCallback() = 0;
  // Commits pending state without a new buffer.
  virtual void commitNative() = 0;
  // True when the platform scales the buffer to the logical size (fractional scale), so non-integer scales apply.
  [[nodiscard]] virtual bool scalesByViewport() const noexcept = 0;
  // Sends the buffer scale or viewport size for the current scale and logical size.
  virtual void applyNativeScale() = 0;
  [[nodiscard]] virtual FrameStatsSink* frameStats(bool create);

  virtual void onConfigure(std::uint32_t width, std::uint32_t height);
  virtual void render();
  [[nodiscard]] bool holdFirstFrame();
  virtual void onFrameCallbackDone();
  // True while onFrameCallbackDone must run after the next frame, so that frame asks for a callback.
  [[nodiscard]] virtual bool wantsFrameDone() const { return false; }
  virtual void onScaleChanged();

  // The platform delivers a frame callback; `callbackMs` is its timestamp in 32-bit milliseconds.
  void frameDone(std::uint32_t callbackMs);
  // Creates the render target once the native surface exists.
  void attachRenderTarget();
  // Seed the surface-local configured scale (/120 numerator) from a known output
  // before first sizing, so explicit-output roles measure at the right scale.
  void setConfiguredScaleNumerator(std::uint32_t numerator) noexcept;
  void updateOutputScale(std::int32_t bufferScale, std::uint32_t configuredScaleNumerator);
  // The compositor's preferred scale for this surface (/120 numerator).
  void setPreferredScaleNumerator(std::uint32_t numerator);
  [[nodiscard]] std::uint32_t preferredScaleNumerator() const noexcept { return m_preferredScaleNumerator; }
  void requestFrame();
  [[nodiscard]] bool needsFrameCallback(std::chrono::steady_clock::time_point now) const;
  // Stops frame work and drops the render target; the platform then destroys the native surface.
  void releaseRenderState();
  void trace(std::string_view event) const;
  [[nodiscard]] static bool traceEnabled();

  void setRunning(bool running) noexcept { m_running = running; }
  void setBufferScale(std::int32_t bufferScale) noexcept { m_bufferScale = bufferScale; }
  [[nodiscard]] bool configured() const noexcept { return m_configured; }

private:
  struct InvalidationToken {};

  void preparePendingFrame();
  void kickFrameLoop();
  void queueFrameWork(bool runFrameTick = false, float deltaMs = 0.0F);
  void cancelQueuedFrameWork();
  void processQueuedFrameWork();
  void queueRenderIfNeeded();
  void continueAnimationFrameLoop();
  void queueRender();
  void cancelQueuedRender();
  void renderQueuedFrame();
  bool ensureRenderTargetReady();
  void resizeRenderTarget();

  RenderContext* m_renderContext = nullptr;
  RenderTarget m_renderTarget;
  AnimationManager* m_animationManager = nullptr;
  Node* m_sceneRoot = nullptr;
  std::optional<WallpaperMaskDrawParams> m_wallpaperMask;
  std::string m_debugName;
  std::string m_accessibleTitle;
  std::shared_ptr<InvalidationToken> m_invalidationToken = std::make_shared<InvalidationToken>();
  ConfigureCallback m_configureCallback;
  PrepareFrameCallback m_prepareFrameCallback;
  UpdateCallback m_updateCallback;
  FrameTickCallback m_frameTickCallback;
  FrameTicks m_frameTicks = FrameTicks::EveryRedraw;
  ScaleChangedCallback m_scaleChangedCallback;
  PresenceChangedCallback m_presenceChangedCallback;
  SurfacePresence m_presence = SurfacePresence::Shown;
  std::function<void()> m_nextPresentCallback;
  bool m_presentedOnce = false;
  // Waiting: no frame yet; Holding: the first frame waits for pending content until m_firstFrameDeadline;
  // Done: a frame presented, or the hold is off.
  enum class FirstFrameHold : std::uint8_t { Waiting, Holding, Done };
  FirstFrameHold m_firstFrameHold = FirstFrameHold::Waiting;
  std::chrono::steady_clock::time_point m_firstFrameDeadline;
  Timer m_firstFrameTimer;
  std::optional<std::chrono::steady_clock::time_point> m_lastFrameAt;
  std::optional<std::chrono::steady_clock::time_point> m_lastPresentAt;
  bool m_updateRequested = false;
  bool m_layoutRequested = false;
  bool m_redrawRequested = false;
  std::uint64_t m_accessibleGeneration = 0; // Node::accessibleGeneration() at the last AT-SPI mark
  bool m_renderHeld = false;
  bool m_running = false;
  float m_resolutionScale = 1.0F;
  bool m_configured = false;
  bool m_inFrameHandler = false;
  bool m_inPrepareFrame = false;
  bool m_frameWorkQueued = false;
  bool m_frameTickPending = false;
  bool m_frameCallbackShouldTick = false;
  bool m_nextFrameCallbackShouldTick = false;
  bool m_renderQueued = false;
  float m_pendingFrameDeltaMs = 0.0F;
  std::optional<std::chrono::steady_clock::time_point> m_pendingFrameTime;
  std::uint32_t m_width = 0;
  std::uint32_t m_height = 0;
  std::int32_t m_bufferScale = 1;
  // Surface-local render scale (/120 numerators). effectiveBufferScale() prefers
  // the compositor override, then the output seed, then integer buffer scale.
  std::uint32_t m_configuredScaleNumerator = 0;
  std::uint32_t m_preferredScaleNumerator = 0;
};
