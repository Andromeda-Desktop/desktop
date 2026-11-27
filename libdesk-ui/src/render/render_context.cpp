#include "render/render_context.h"


#include "core/files/resource_paths.h"
#include "core/log.h"
#include "core/scoped_timer.h"
#include "core/ui_phase.h"
#include "render/backend/render_backend.h"
#include "render/core/texture_handle.h"
#include "render/core/texture_manager.h"
#include "render/core/wallpaper_types.h"
#include "render/gl_shared_context.h"
#include "render/render_target.h"
#include "render/scene/audio_spectrum_node.h"
#include "render/scene/countdown_ring_node.h"
#include "render/scene/effect_node.h"
#include "render/scene/fancy_audio_visualizer_node.h"
#include "render/scene/glyph_node.h"
#include "render/scene/graph_node.h"
#include "render/scene/image_node.h"
#include "render/scene/texture_shader_node.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "render/scene/screen_corner_node.h"
#include "render/scene/spinner_node.h"
#include "render/scene/shader_scene_node.h"
#include "render/scene/terminal_grid_node.h"
#include "render/scene/text_node.h"
#include "render/scene/wallpaper_node.h"

#include <algorithm>
#include <chrono>
#include <fontconfig/fontconfig.h>
#include <optional>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace {

  // Logical px a shadow blurred by `blur` reaches past its offset copy: blurReach(round(blur x scale)) / scale stays
  // within 1.5 x blur + 3 at every scale >= 1.
  float shadowReach(float blur) noexcept { return blur > 0.0F ? 1.5F * blur + 3.0F : 0.0F; }

  constexpr Logger kLog("render");
  constexpr float kSlowRenderOperationDebugMs = 50.0F;
  constexpr float kSlowRenderOperationWarnMs = 1000.0F;
  constexpr float kPaintCullSlack = 16.0F;
  constexpr float kAxisAlignedEpsilon = 0.0001F;

  bool isAxisAligned(const Mat3& transform) {
    return std::abs(transform.m[1]) <= kAxisAlignedEpsilon && std::abs(transform.m[3]) <= kAxisAlignedEpsilon;
  }

  float snapToBufferPixel(float value, float scale) {
    const float safeScale = std::max(1.0F, scale);
    return std::round(value * safeScale) / safeScale;
  }
  // A node's sub-rect damage already holds its content overhang; one pixel covers edge antialiasing.
  constexpr float kPartialDamageSlack = 1.0F;
  constexpr auto kGpuCacheIdleExpiry = std::chrono::seconds(15);
  constexpr auto kCacheExpirySlack = std::chrono::seconds(1);

} // namespace

namespace {

  float elapsedSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
  }

  template <typename... Args> void logSlowRenderOperation(float ms, std::format_string<Args...> fmt, Args&&... args) {
    if (ms >= kSlowRenderOperationWarnMs) {
      kLog.warn(fmt, std::forward<Args>(args)...);
    } else if (ms >= kSlowRenderOperationDebugMs) {
      kLog.debug(fmt, std::forward<Args>(args)...);
    }
  }

  RenderScissor
  scissorForClip(float sw, float sh, float bw, float bh, float left, float top, float right, float bottom) {
    const float scaleX = sw > 0.0F ? bw / sw : 1.0F;
    const float scaleY = sh > 0.0F ? bh / sh : 1.0F;
    const auto clampX = [bw](std::int32_t value) {
      return std::clamp(value, std::int32_t{0}, static_cast<std::int32_t>(std::ceil(bw)));
    };
    const auto clampY = [bh](std::int32_t value) {
      return std::clamp(value, std::int32_t{0}, static_cast<std::int32_t>(std::ceil(bh)));
    };

    // Round clip edges independently in buffer space. Rounding the size after
    // flooring the origin can drop the final column/row for fractional origins.
    const std::int32_t x0 = clampX(static_cast<std::int32_t>(std::floor(left * scaleX)));
    const std::int32_t x1 = clampX(static_cast<std::int32_t>(std::ceil(right * scaleX)));
    const std::int32_t y0 = clampY(static_cast<std::int32_t>(std::floor((sh - bottom) * scaleY)));
    const std::int32_t y1 = clampY(static_cast<std::int32_t>(std::ceil((sh - top) * scaleY)));
    return RenderScissor{
        .x = x0,
        .y = y0,
        .width = std::max(std::int32_t{0}, x1 - x0),
        .height = std::max(std::int32_t{0}, y1 - y0),
    };
  }

  RenderScissor intersectScissors(const RenderScissor& a, const RenderScissor& b) {
    const std::int32_t x0 = std::max(a.x, b.x);
    const std::int32_t y0 = std::max(a.y, b.y);
    return RenderScissor{
        .x = x0,
        .y = y0,
        .width = std::max(std::int32_t{0}, std::min(a.x + a.width, b.x + b.width) - x0),
        .height = std::max(std::int32_t{0}, std::min(a.y + a.height, b.y + b.height) - y0),
    };
  }

  template <typename Fn> void forEachChildInPaintOrder(const Node* node, Fn&& fn) {
    for (const Node* child : node->childrenInPaintOrder()) {
      fn(child);
    }
  }

  // True when `area` reaches outside a corner arc of `box` shrunk by `inset` (radii shrink with it); 1 px covers the
  // edge. The point of `area` nearest a box corner is the one farthest outside that corner's arc.
  bool reachesOutsideCorners(DamageRect box, const Radii& radii, float scale, float inset, const DamageRect& area) {
    box = {box.x0 + inset, box.y0 + inset, box.x1 - inset, box.y1 - inset};
    const DamageRect a{std::max(box.x0, area.x0), std::max(box.y0, area.y0), std::min(box.x1, area.x1),
                       std::min(box.y1, area.y1)};
    if (a.empty()) {
      return false;
    }
    const auto outside = [&](float radius, float px, float py, float cornerX, float cornerY) {
      const float rr = (radius * scale) - inset;
      const float cx = cornerX + std::copysign(rr, px - cornerX);
      const float cy = cornerY + std::copysign(rr, py - cornerY);
      const bool inSquare = std::abs(px - cornerX) < rr && std::abs(py - cornerY) < rr;
      return rr > 0.0F && inSquare && std::hypot(px - cx, py - cy) > rr - 1.0F;
    };
    return outside(radii.tl, a.x0, a.y0, box.x0, box.y0) || outside(radii.tr, a.x1, a.y0, box.x1, box.y0)
        || outside(radii.br, a.x1, a.y1, box.x1, box.y1) || outside(radii.bl, a.x0, a.y1, box.x0, box.y1);
  }

  // True when the node fills all of `area` with fully opaque pixels. Only solid, axis-aligned rects at full opacity
  // count; the cover stays inside the corners, the border and the anti-aliased edge.
  bool coversOpaque(const Node* node, const Mat3& world, float opacity, const DamageRect& bounds, const DamageRect& area) {
    if (opacity < 1.0F || world.m[1] != 0.0F || world.m[3] != 0.0F || world.m[0] <= 0.0F || world.m[4] <= 0.0F) {
      return false;
    }
    if (node->type() == NodeType::TerminalGrid) {
      // The background fill (TerminalGridProgram::drawFill) paints the node box unblended. A pass is whole buffer
      // pixels, so inside the box every pixel centre is.
      return static_cast<const TerminalGridNode*>(node)->backgroundAlpha() >= 1.0F && bounds.contains(area);
    }
    if (node->type() != NodeType::Rect) {
      return false;
    }
    const RoundedRectStyle& style = static_cast<const RectNode*>(node)->style();
    if (style.fillMode != FillMode::Solid || style.fill.a < 1.0F || style.invertFill || style.outerShadow != OuterShadow::None
        || style.shadowExclusion || style.logicalInset != RectInsets{} || style.corners != CornerShapes{}) {
      return false;
    }
    const float scale = std::min(world.m[0], world.m[4]);
    const float inset = (style.borderWidth + style.softness + 1.0F) * scale;
    const DamageRect cover{bounds.x0 + inset, bounds.y0 + inset, bounds.x1 - inset, bounds.y1 - inset};
    return cover.contains(area) && !reachesOutsideCorners(bounds, style.radius, scale, inset, area);
  }


  std::optional<DamageRect> paintBounds(const Node* node, const Mat3& world) {
    if (node->type() == NodeType::Base) {
      return std::nullopt;
    }
    float left = 0.0F;
    float top = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;
    Node::transformedBounds(node, world, left, top, right, bottom);
    float pad = kPaintCullSlack;
    if (node->type() == NodeType::Text) {
      const auto* text = static_cast<const TextNode*>(node);
      // A text node has no size: it paints its measured extent around the baseline origin.
      const DamageRect& e = text->paintExtent();
      for (const Vec2 corner :
           {world.transformPoint(e.x0, e.y0), world.transformPoint(e.x1, e.y0), world.transformPoint(e.x1, e.y1),
            world.transformPoint(e.x0, e.y1)}) {
        left = std::min(left, corner.x);
        top = std::min(top, corner.y);
        right = std::max(right, corner.x);
        bottom = std::max(bottom, corner.y);
      }
      pad += std::max(std::abs(text->shadowOffsetX()), std::abs(text->shadowOffsetY())) + shadowReach(text->shadowBlur());
    } else if (node->type() == NodeType::Rect) {
      // Same overhang as the rect quad (RectProgram::draw: border + softness + 2 px).
      const auto& style = static_cast<const RectNode*>(node)->style();
      pad = std::max(pad, style.borderWidth + style.softness + 2.0F);
    } else if (node->type() == NodeType::Glyph) {
      const auto* glyph = static_cast<const GlyphNode*>(node);
      // A glyph node has no size: its ink sits in the em box above and right of the baseline origin, plus half an em.
      const float s = glyph->fontSize();
      for (const Vec2 corner : {world.transformPoint(-0.5F * s, -1.5F * s), world.transformPoint(1.5F * s, -1.5F * s),
                                world.transformPoint(1.5F * s, 0.5F * s), world.transformPoint(-0.5F * s, 0.5F * s)}) {
        left = std::min(left, corner.x);
        top = std::min(top, corner.y);
        right = std::max(right, corner.x);
        bottom = std::max(bottom, corner.y);
      }
      pad += std::max(std::abs(glyph->shadowOffsetX()), std::abs(glyph->shadowOffsetY())) + shadowReach(glyph->shadowBlur());
    }
    // The pads above are local units; a scaled or rotated node moves its overhang up to its largest axis scale.
    const Vec2 o = world.transformPoint(0.0F, 0.0F);
    const Vec2 ax = world.transformPoint(1.0F, 0.0F);
    const Vec2 ay = world.transformPoint(0.0F, 1.0F);
    pad *= std::max(1.0F, std::max(std::hypot(ax.x - o.x, ax.y - o.y), std::hypot(ay.x - o.x, ay.y - o.y)));
    return DamageRect{left - pad, top - pad, right + pad, bottom + pad};
  }

  // Adds what changed under `node` since the last frame and records the new painted bounds. A node whose own paint
  // changed (position, opacity, style) repaints its whole subtree. False when the frame needs a full repaint.
  // `clip`: ancestors' clipChildren area, old and new united; pixels outside it never change.
  bool collectDamage(
      Node* node, const Mat3& parentTransform, bool subtreeChanged, const std::optional<DamageRect>& clip,
      DamageRegion& out
  ) {
    if (!subtreeChanged && !node->paintDirty() && !node->layoutDirty()) {
      return true;
    }
    if (!node->visible()) {
      Node::takePaintedBounds(node, out);
      return true;
    }
    if (node->type() == NodeType::RenderProxy) {
      return false;
    }
    node->markPaintedSubtree();
    const Mat3 world = parentTransform * node->localTransform();
    const std::optional<DamageRect> bounds = paintBounds(node, world);
    const bool whole = subtreeChanged || node->selfPaintDirty();
    const auto add = [&clip, &out](const DamageRect& r) {
      const DamageRect cut = clip ? DamageRect{std::max(r.x0, clip->x0), std::max(r.y0, clip->y0),
                                               std::min(r.x1, clip->x1), std::min(r.y1, clip->y1)}
                                  : r;
      if (!cut.empty()) {
        out.add(cut);
      }
    };
    // The descendants' painted bounds may be stale: repaint the area that holds their pixels and refresh them all.
    const std::optional<DamageRect> staleArea = node->childArea();
    if (staleArea) {
      add(*staleArea);
      node->setChildArea(std::nullopt);
    }
    if (whole || node->paintedBounds() != bounds) {
      if (node->paintedBounds()) {
        add(*node->paintedBounds());
      }
      if (bounds) {
        add(*bounds);
      }
      node->setPaintedBounds(bounds);
    } else if (const DamageRegion* partial = node->partialDamage(); partial != nullptr) {
      for (const DamageRect& r : partial->rects()) {
        DamageRect w{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                     std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
        for (const Vec2 c : {world.transformPoint(r.x0, r.y0), world.transformPoint(r.x1, r.y0),
                             world.transformPoint(r.x1, r.y1), world.transformPoint(r.x0, r.y1)}) {
          w = {std::min(w.x0, c.x), std::min(w.y0, c.y), std::max(w.x1, c.x), std::max(w.y1, c.y)};
        }
        add(DamageRect{w.x0 - kPartialDamageSlack, w.y0 - kPartialDamageSlack, w.x1 + kPartialDamageSlack,
                       w.y1 + kPartialDamageSlack});
      }
    }
    std::optional<DamageRect> childClip = clip;
    if (node->clipChildren()) {
      // Same rect renderNode scissors children to. Until the node has a recorded clip, children stay unclipped here.
      DamageRect box;
      Node::transformedBounds(node, world, box.x0, box.y0, box.x1, box.y1);
      if (const std::optional<DamageRect>& previous = node->paintedClip(); previous) {
        const DamageRect both = uniteRects(*previous, box);
        childClip = clip ? DamageRect{std::max(both.x0, clip->x0), std::max(both.y0, clip->y0),
                                      std::min(both.x1, clip->x1), std::min(both.y1, clip->y1)}
                         : both;
      }
      node->setPaintedClip(box);
    }
    // A moved node whose box fills its children's clip (scrolled content) repaints that whole clip anyway: record it
    // as the child area, and from the second such frame on skip the walk over every descendant.
    if (childClip && node->selfPaintDirty() && !node->children().empty()) {
      DamageRect box;
      Node::transformedBounds(node, world, box.x0, box.y0, box.x1, box.y1);
      if (box.contains(*childClip)) {
        node->setChildArea(childClip);
        if (staleArea) {
          add(*childClip);
          return true;
        }
      }
    }
    for (const auto& child : node->children()) {
      if (!collectDamage(child.get(), world, whole || staleArea.has_value(), childClip, out)) {
        return false;
      }
    }
    return true;
  }

} // namespace

RenderContext::RenderContext() = default;

RenderContext::~RenderContext() { cleanup(); }

void RenderContext::initialize(GlSharedContext& shared) {
  initializeText();
  attachGl(shared);
}

void RenderContext::initializeText() {
  cleanup();
  // Pango handles font fallback via Fontconfig automatically — no explicit chain.
  m_textRenderer.initialize(nullptr, nullptr);
  m_glyphRenderer.initialize(paths::assetPath("fonts/noctalia-tabler.ttf").string(), nullptr, nullptr);
  m_textRenderer.setFontFamily(m_textFontFamily);
  m_textRenderer.setBaseDirection(m_textBaseDirRtl);
  ++m_textMetricsVersion;
}

void RenderContext::attachGlLater(std::function<void()> attach) {
  if (m_glState != GlState::None) {
    throw std::logic_error("RenderContext: GL is already attached or pending");
  }
  m_glAttach = std::move(attach);
  m_glState = GlState::Pending;
}

void RenderContext::attachGl(GlSharedContext& shared) {
  if (m_glState == GlState::Ready) {
    throw std::logic_error("RenderContext: GL is already attached");
  }
  m_shared = &shared;
  m_backend = createDefaultRenderBackend();
  m_backend->initialize(shared);
  m_backend->textureManager().probeExtensions();
  m_textRenderer.attachBackend(m_backend.get(), &m_backend->textureManager());
  m_glyphRenderer.attachBackend(m_backend.get(), &m_backend->textureManager());
  m_graphicsResetPending = false;
  m_glState = GlState::Ready;
}

bool RenderContext::ensureGl() {
  if (m_glState == GlState::Pending) {
    const std::function<void()> attach = std::exchange(m_glAttach, {});
    m_glState = GlState::Failed; // attach sets Ready through attachGl()
    attach();
  }
  return m_glState == GlState::Ready;
}

RenderBackend& RenderContext::backend() {
  ensureGl();
  return *m_backend;
}

void RenderContext::restoreAfterGraphicsReset(GlSharedContext& shared) {
  if (m_backend == nullptr) {
    throw std::runtime_error("cannot restore an uninitialized render context");
  }
  m_shared = &shared;
  m_backend->initialize(shared);
  m_backend->textureManager().probeExtensions();
  invalidateGpuResourcesNextFrame();
}

void RenderContext::prepareForGraphicsReset() {
  if (m_backend == nullptr) {
    return;
  }

  m_textRenderer.abandonGlyphTextures();
  m_glyphRenderer.abandonGlyphTextures();
  m_backend->abandonAfterGraphicsReset();
}

void RenderContext::expireIdleCaches() {
  const auto now = std::chrono::steady_clock::now();
  if (!makeCurrentNoSurface()) {
    return;
  }
  m_textRenderer.evictUnusedBefore(now - kGpuCacheIdleExpiry);
  m_glyphRenderer.evictUnusedBefore(now - kGpuCacheIdleExpiry);
  // Entries the last frame used expire later: one more wake-up, then the timer stays off until the next frame.
  const auto next = m_lastSceneDraw + kGpuCacheIdleExpiry + kCacheExpirySlack;
  if (next > now) {
    m_cacheExpiryTimer.start(std::chrono::ceil<std::chrono::milliseconds>(next - now), [this] { expireIdleCaches(); });
  }
}

bool RenderContext::makeCurrentNoSurface() {
  if (!ensureGl() || m_graphicsResetPending) {
    return false;
  }
  return m_backend->makeCurrentNoSurface();
}

void RenderContext::makeCurrentIfGlReady() {
  if (m_glState == GlState::Ready) {
    makeCurrentNoSurface();
  }
}

bool RenderContext::makeCurrent(RenderTarget& target) {
  return m_backend != nullptr && !m_graphicsResetPending && m_backend->makeCurrent(target);
}

bool RenderContext::readPixels(RenderTarget& target, std::span<std::uint8_t> rgba) {
  return m_backend != nullptr && !m_graphicsResetPending && m_backend->readPixels(target, rgba);
}

TextureId RenderContext::finishedOffscreenTexture(RenderTarget& target) {
  return m_backend != nullptr && !m_graphicsResetPending ? m_backend->finishedOffscreenTexture(target) : TextureId{};
}

void RenderContext::setTextFontFamily(std::string family) {
  if (family.empty()) {
    family = "sans-serif";
  }
  if (m_textFontFamily == family) {
    return;
  }
  makeCurrentIfGlReady();
  m_textFontFamily = std::move(family);
  m_textRenderer.setFontFamily(m_textFontFamily);
  ++m_textMetricsVersion;
}

void RenderContext::setTextScale(float scale) {
  makeCurrentIfGlReady();
  m_textRenderer.setTextScale(scale);
  m_textScale = scale;
  ++m_textMetricsVersion;
}

void RenderContext::setTextBaseDirection(bool rtl) {
  if (m_textBaseDirRtl == rtl) {
    return;
  }
  makeCurrentIfGlReady();
  m_textBaseDirRtl = rtl;
  m_textRenderer.setBaseDirection(rtl);
  ++m_textMetricsVersion;
}

void RenderContext::notifyFontConfigChanged() {
  m_textRenderer.notifyFontConfigChanged();
  ++m_textMetricsVersion;
}

bool RenderContext::renderScene(RenderTarget& target, Node* sceneRoot, const WallpaperMaskDrawParams* wallpaperMask) {
  if (m_backend == nullptr || m_graphicsResetPending) {
    return false;
  }
  const auto totalStart = std::chrono::steady_clock::now();
  // Sensitive text rasters from the previous frame: that frame is submitted, so their textures can go.
  m_textRenderer.releaseTransient();
  const auto sw = static_cast<float>(target.logicalWidth());
  const auto sh = static_cast<float>(target.logicalHeight());
  const auto bw = static_cast<float>(target.bufferWidth());
  const auto bh = static_cast<float>(target.bufferHeight());
  const DamageRect surfaceRect{0.0F, 0.0F, sw, sh};

  // A full repaint is needed when the buffer changed, GPU resources or glyphs were rebuilt, or the wallpaper mask
  // changed; an unchanged mask is redrawn inside each repainted rect.
  // ANDROMEDA_FULL_REDRAW turns damage tracking off, to rule it out when a surface shows stale pixels.
  static const bool forceFull = (std::getenv("ANDROMEDA_FULL_REDRAW") ? std::getenv("ANDROMEDA_FULL_REDRAW") : std::getenv("NOCTALIA_FULL_REDRAW")) != nullptr;
  const bool damageReset = target.takeDamageReset(sceneRoot);
  const bool maskChanged = target.takeWallpaperMaskChange(wallpaperMask);
  // Why this frame repaints everything (ANDROMEDA_PROFILE frame stats); the first reason found wins.
  const char* fullReason = forceFull ? "env"
      : damageReset                  ? "reset"
      : sceneRoot == nullptr         ? "no-root"
      : maskChanged                  ? "wallpaper-mask"
      : m_glyphTexturesDirty         ? "glyphs"
      : (m_gpuResourceVersion != 0 && sceneRoot->gpuResourceGeneration() != m_gpuResourceVersion) ? "gpu-reset"
                                     : nullptr;
  bool full = fullReason != nullptr;
  DamageRegion damage;
  if (sceneRoot != nullptr) {
    damage = sceneRoot->takePendingDamage();
    if (!collectDamage(sceneRoot, Mat3::identity(), false, std::nullopt, damage) && !full) {
      full = true;
      fullReason = "render-proxy";
    }
  }
  damage.clipTo(surfaceRect);
  if (!full && damage.empty()) {
    return false;
  }

  if (!m_backend->beginFrame(target)) {
    // Collected damage is lost with this frame: make the next one a full repaint.
    (void)target.takeDamageReset(nullptr);
    return false;
  }
  const float renderScale = target.contentScale();
  std::optional<DamageRegion> repaint;
  const int bufferAge = full ? -1 : m_backend->bufferAge(target);
  if (!full) {
    repaint = target.damageHistory().regionFor(bufferAge, damage);
    if (!repaint) {
      fullReason = "buffer-age";
    }
  }
  if (full) {
    damage.clear();
    damage.add(surfaceRect);
  }
  target.damageHistory().push(damage);

  if (sceneRoot != nullptr
      && m_gpuResourceVersion != 0
      && sceneRoot->gpuResourceGeneration() != m_gpuResourceVersion) {
    sceneRoot->invalidateGpuResources(target.renderer(), m_gpuResourceVersion);
  }

  if (m_glyphTexturesDirty) {
    m_textRenderer.invalidateGlyphTextures();
    m_glyphRenderer.invalidateGlyphTextures();
    m_glyphTexturesDirty = false;
  }

  const auto drawStart = std::chrono::steady_clock::now();
  m_statDrawn = 0;
  m_statCulled = 0;
  std::vector<DamageRect> bufferDamage;
  {
    UiPhaseScope renderPhase(UiPhase::Render);
    if (!repaint) {
      m_backend->clear(rgba(0.0F, 0.0F, 0.0F, 0.0F));
      if (sceneRoot != nullptr) {
        renderNode(
            renderScale, sceneRoot, Mat3::identity(), 1.0F, sw, sh, bw, bh, 0.0F, 0.0F, sw, sh, false, false, false
        );
      }
    } else {
      const float sx = sw > 0.0F ? bw / sw : 1.0F;
      const float sy = sh > 0.0F ? bh / sh : 1.0F;
      const DamageRegion pixels = repaint->toBufferPixels(sx, sy);
      DamageRect bounds;
      m_passCount = 0;
      m_backend->setBlendMode(RenderBlendMode::PremultipliedAlpha);
      for (const DamageRect& buffer : pixels.rects()) {
        bufferDamage.push_back(buffer);
        const RenderScissor scissor{
            .x = static_cast<std::int32_t>(buffer.x0),
            .y = static_cast<std::int32_t>(bh - buffer.y1),
            .width = static_cast<std::int32_t>(buffer.x1 - buffer.x0),
            .height = static_cast<std::int32_t>(buffer.y1 - buffer.y0),
        };
        const DamageRect logical{buffer.x0 / sx, buffer.y0 / sy, buffer.x1 / sx, buffer.y1 / sy};
        m_passes[m_passCount++] = {.scissor = scissor, .logical = logical};
        bounds = uniteRects(bounds, logical);
      }
      if (sceneRoot != nullptr) {
        findOccluders(sceneRoot, Mat3::identity(), 1.0F, bounds, 0);
      }
      // An occluder paints every pixel of its pass opaque; a pass whose occluder does not draw is cleared below.
      for (std::size_t i = 0; i < m_passCount; ++i) {
        if (m_passes[i].occluder == nullptr) {
          m_backend->setScissor(m_passes[i].scissor);
          m_backend->clear(rgba(0.0F, 0.0F, 0.0F, 0.0F));
        }
      }
      renderNode(
          renderScale, sceneRoot, Mat3::identity(), 1.0F, sw, sh, bw, bh, bounds.x0, bounds.y0, bounds.x1, bounds.y1,
          true, false, false
      );
      // A pass whose occluder did not draw (culled, clipped out) drew nothing: draw it again in full.
      const auto drawnPasses = m_passes;
      const std::size_t passCount = m_passCount;
      m_passCount = 0;
      for (std::size_t i = 0; i < passCount; ++i) {
        if (drawnPasses[i].occluder != nullptr && !drawnPasses[i].occluderReached) {
          m_passes[m_passCount] = drawnPasses[i];
          m_passes[m_passCount++].occluder = nullptr;
        }
      }
      if (m_passCount > 0) {
        kLog.debug("occluder not drawn: {} passes drawn again", m_passCount);
        for (std::size_t i = 0; i < m_passCount; ++i) {
          m_backend->setScissor(m_passes[i].scissor);
          m_backend->clear(rgba(0.0F, 0.0F, 0.0F, 0.0F));
        }
        renderNode(
            renderScale, sceneRoot, Mat3::identity(), 1.0F, sw, sh, bw, bh, bounds.x0, bounds.y0, bounds.x1,
            bounds.y1, true, false, false
        );
      }
      m_passes = drawnPasses;
      m_passCount = passCount;
      // The mask cuts the repainted pixels only; pixels outside the passes keep their earlier cut.
      if (wallpaperMask != nullptr && wallpaperMask->texture != 0) {
        m_backend->setBlendMode(RenderBlendMode::DestinationOut);
        for (std::size_t i = 0; i < m_passCount; ++i) {
          m_backend->setScissor(m_passes[i].scissor);
          m_backend->drawWallpaperMask(*wallpaperMask);
        }
      }
      m_passCount = 0;
    }
    if (!repaint && wallpaperMask != nullptr && wallpaperMask->texture != 0) {
      m_backend->disableScissor();
      m_backend->setBlendMode(RenderBlendMode::DestinationOut);
      m_backend->drawWallpaperMask(*wallpaperMask);
    }
  }
  float ms = elapsedSince(drawStart);
  logSlowRenderOperation(
      ms, "scene draw took {:.1F}ms ({}x{} logical, {}x{} buffer)", ms, target.logicalWidth(), target.logicalHeight(),
      target.bufferWidth(), target.bufferHeight()
  );

  // Drained every frame so a profiled frame shows only its own merged draws.
  const RenderBackend::BatchStats batch = m_backend->takeBatchStats();
  if (noctalia::profiling::enabled()) {
    float area = 0.0F;
    for (const DamageRect& r : repaint ? repaint->rects() : damage.rects()) {
      area += r.area();
    }
    kLog.info(
        "frame-stats {}x{} age={} rects={} full={} area={:.0F}% drawn={} culled={} batched={}/{} draw_ms={:.3F}",
        target.logicalWidth(), target.logicalHeight(), bufferAge, repaint ? repaint->rects().size() : std::size_t{0},
        fullReason != nullptr ? fullReason : "-", sw * sh > 0.0F ? area * 100.0F / (sw * sh) : 0.0F, m_statDrawn,
        m_statCulled, batch.rects, batch.draws, ms
    );
  }
  // GPU caches drop what no frame drew for this long, not only what the LRU caps push out (GTK GSK).
  m_lastSceneDraw = std::chrono::steady_clock::now();
  m_textRenderer.evictUnusedBefore(m_lastSceneDraw - kGpuCacheIdleExpiry);
  m_glyphRenderer.evictUnusedBefore(m_lastSceneDraw - kGpuCacheIdleExpiry);
  if (!m_cacheExpiryTimer.active()) {
    m_cacheExpiryTimer.start(
        std::chrono::duration_cast<std::chrono::milliseconds>(kGpuCacheIdleExpiry + kCacheExpirySlack),
        [this] { expireIdleCaches(); }
    );
  }
  m_backend->endFrame(target, bufferDamage);
  const RenderGraphicsResetStatus resetStatus = m_backend->graphicsResetStatus();
  ms = elapsedSince(totalStart);
  logSlowRenderOperation(ms, "renderScene took {:.1F}ms total", ms);
  if (resetStatus != RenderGraphicsResetStatus::NoError) {
    handleGraphicsReset(resetStatus);
  }
  return true;
}

TextMetrics RenderContext::measureTextScaled(
    float scale, std::string_view text, float fontSize, const TextFont& font, float maxWidth, int maxLines,
    TextAlign align, TextEllipsize ellipsize, bool useMarkup
) {
  auto m = m_textRenderer.measure(scale, text, fontSize, font, maxWidth, maxLines, align, ellipsize, useMarkup);
  return TextMetrics{
      .width = m.width,
      .left = m.left,
      .right = m.right,
      .top = m.top,
      .bottom = m.bottom,
      .inkTop = m.inkTop,
      .inkBottom = m.inkBottom,
      .inkLeft = m.inkLeft,
      .inkRight = m.inkRight,
      .lineCount = m.lineCount,
      .alignOffsetX = m.alignOffsetX,
      .ellipsized = m.ellipsized
  };
}

TextMetrics RenderContext::measureFontScaled(float scale, float fontSize, const TextFont& font) {
  auto m = m_textRenderer.measureFont(scale, fontSize, font);
  return TextMetrics{
      .width = m.width,
      .left = m.left,
      .right = m.right,
      .top = m.top,
      .bottom = m.bottom,
      .inkTop = m.inkTop,
      .inkBottom = m.inkBottom,
      .inkLeft = m.inkLeft,
      .inkRight = m.inkRight,
      .capHeight = m.capHeight
  };
}

void RenderContext::measureTextCursorStopsScaled(
    float scale, std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets,
    std::vector<float>& outStops, const TextFont& font
) {
  m_textRenderer.measureCursorStops(scale, text, fontSize, byteOffsets, outStops, font);
}

void RenderContext::measureTextCursorStopsWrappedScaled(
    float scale, std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, float maxWidth,
    std::vector<TextCursorStop>& outStops, const TextFont& font
) {
  m_textRenderer.measureCursorStopsWrapped(scale, text, fontSize, byteOffsets, maxWidth, outStops, font);
}

void RenderContext::measureTextCharSpansScaled(
    float scale, std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets,
    std::vector<TextCharSpan>& outSpans, const TextFont& font
) {
  m_textRenderer.measureCharSpans(scale, text, fontSize, byteOffsets, outSpans, font);
}

TextMetrics RenderContext::measureGlyphScaled(float scale, char32_t codepoint, float fontSize) {
  auto m = m_glyphRenderer.measureGlyph(scale, codepoint, fontSize);
  return TextMetrics{
      .width = m.width,
      .left = m.left,
      .right = m.right,
      .top = m.top,
      .bottom = m.bottom,
      .inkTop = m.top,
      .inkBottom = m.bottom,
      .inkLeft = m.left,
      .inkRight = m.right
  };
}

void RenderContext::releaseUnusedCaches() {
  if (makeCurrentNoSurface()) {
    const auto now = std::chrono::steady_clock::now();
    m_textRenderer.evictUnusedBefore(now);
    m_glyphRenderer.evictUnusedBefore(now);
  }
  m_unusedCacheRelease.emit();
}

TextureManager& RenderContext::textureManager() {
  makeCurrentNoSurface();
  return m_backend->textureManager();
}

void RenderContext::invalidateGpuResourcesNextFrame() noexcept {
  ++m_gpuResourceVersion;
  m_glyphTexturesDirty = true;
}

void RenderContext::handleGraphicsReset(RenderGraphicsResetStatus status) {
  if (m_graphicsResetPending) {
    return;
  }
  m_graphicsResetPending = true;
  m_shared->reportGraphicsReset(status, "render");
}

// Paint order as renderNode: own draw, then children. A later cover replaces an earlier one, so each pass keeps the
// topmost. A pass that reaches outside an ancestor's rounded clip is blocked: the clip cuts the cover's corners.
void RenderContext::findOccluders(const Node* node, const Mat3& parentTransform, float parentOpacity, DamageRect clip,
                                  std::uint32_t blockedPasses) {
  if (!node->visible() || node->type() == NodeType::RenderProxy) {
    return;
  }
  const Mat3 world = parentTransform * node->localTransform();
  const float opacity = parentOpacity * node->opacity();
  if (opacity <= 0.0F) {
    return;
  }
  DamageRect bounds;
  Node::transformedBounds(node, world, bounds.x0, bounds.y0, bounds.x1, bounds.y1);
  for (std::size_t i = 0; i < m_passCount; ++i) {
    const DamageRect& area = m_passes[i].logical;
    if ((blockedPasses & (1U << i)) == 0 && clip.contains(area) && coversOpaque(node, world, opacity, bounds, area)) {
      m_passes[i].occluder = node;
    }
  }
  if (node->clipChildren()) {
    clip = {std::max(clip.x0, bounds.x0), std::max(clip.y0, bounds.y0), std::min(clip.x1, bounds.x1),
            std::min(clip.y1, bounds.y1)};
    if (clip.empty()) {
      return;
    }
  }
  if (const std::optional<Radii>& round = node->childClip(); round) {
    const bool axisAligned = world.m[1] == 0.0F && world.m[3] == 0.0F;
    const float scale = std::min(std::abs(world.m[0]), std::abs(world.m[4]));
    for (std::size_t i = 0; i < m_passCount; ++i) {
      if (!axisAligned || reachesOutsideCorners(bounds, *round, scale, 0.0F, m_passes[i].logical)) {
        blockedPasses |= 1U << i;
      }
    }
  }
  // The clip only shrinks and blocked passes only grow below here: with no open pass left, no descendant can occlude.
  bool open = false;
  for (std::size_t i = 0; i < m_passCount && !open; ++i) {
    open = (blockedPasses & (1U << i)) == 0 && clip.contains(m_passes[i].logical);
  }
  if (!open) {
    return;
  }
  forEachChildInPaintOrder(node, [&](const Node* child) { findOccluders(child, world, opacity, clip, blockedPasses); });
}

void RenderContext::renderNode(
    float renderScale, const Node* node, const Mat3& parentTransform, float parentOpacity, float sw, float sh, float bw,
    float bh, float clipLeft, float clipTop, float clipRight, float clipBottom, bool hasClip, bool ignoreNodeOpacity,
    bool parentPaintContained
) {
  if (!node->visible()) {
    return;
  }

  const Mat3 worldTransform = parentTransform * node->localTransform();
  const float effectiveOpacity = ignoreNodeOpacity ? parentOpacity : parentOpacity * node->opacity();
  float boundsLeft = 0.0F;
  float boundsTop = 0.0F;
  float boundsRight = 0.0F;
  float boundsBottom = 0.0F;
  Node::transformedBounds(node, worldTransform, boundsLeft, boundsTop, boundsRight, boundsBottom);

  const bool paintContained = parentPaintContained || node->paintContained();
  const auto boundsMiss = [&](const DamageRect& r) {
    return boundsRight + kPaintCullSlack <= r.x0
        || boundsLeft - kPaintCullSlack >= r.x1
        || boundsBottom + kPaintCullSlack <= r.y0
        || boundsTop - kPaintCullSlack >= r.y1;
  };
  const DamageRect clip{clipLeft, clipTop, clipRight, clipBottom};
  const std::span<const RepaintPass> passes{m_passes.data(), m_passCount};
  if (paintContained
      && hasClip
      && node->type() != NodeType::RenderProxy
      && node->width() > 0.0F
      && node->height() > 0.0F
      && (boundsMiss(clip) || (m_passCount > 0 && std::ranges::all_of(passes, boundsMiss, &RepaintPass::logical)))) {
    ++m_statCulled;
    return;
  }

  if (node->type() == NodeType::RenderProxy) {
    ++m_statDrawn;
    const auto* proxy = static_cast<const RenderProxyNode*>(node);
    const Node* source = proxy->source();
    bool sourceContainsProxy = false;
    for (const Node* current = node; current != nullptr; current = current->parent()) {
      if (current == source) {
        sourceContainsProxy = true;
        break;
      }
    }
    if (source != nullptr && !sourceContainsProxy) {
      const Mat3 sourceParent = worldTransform * Mat3::translation(-source->x(), -source->y());
      renderNode(
          renderScale, source, sourceParent, effectiveOpacity, sw, sh, bw, bh, clipLeft, clipTop, clipRight, clipBottom,
          hasClip, true, false
      );
    }
    return;
  }

  float childClipLeft = clipLeft;
  float childClipTop = clipTop;
  float childClipRight = clipRight;
  float childClipBottom = clipBottom;
  bool childHasClip = hasClip;

  if (node->clipChildren()) {
    childClipLeft = hasClip ? std::max(childClipLeft, boundsLeft) : boundsLeft;
    childClipTop = hasClip ? std::max(childClipTop, boundsTop) : boundsTop;
    childClipRight = hasClip ? std::min(childClipRight, boundsRight) : boundsRight;
    childClipBottom = hasClip ? std::min(childClipBottom, boundsBottom) : boundsBottom;
    childHasClip = true;
  }
  const bool childrenClipped = childHasClip && (childClipRight <= childClipLeft || childClipBottom <= childClipTop);

  // The node's own draw is skipped where it misses the clip; its children may still reach it. Same bounds as damage.
  const std::optional<DamageRect> paint = node->type() != NodeType::Base && hasClip
      ? paintBounds(node, worldTransform)
      : std::nullopt;
  const auto drawSelf = [&](std::uint8_t gridPhases) {
    if (node->type() == NodeType::Base) {
      return;
    }
    const auto reaches = [&](const DamageRect& r) {
      return paint && paint->overlaps(r);
    };
    bool drawn = false;
    if (!hasClip) {
      m_backend->disableScissor();
      drawNodeSelf(renderScale, node, worldTransform, effectiveOpacity, sw, sh, bw, bh, gridPhases);
      drawn = true;
    } else if (reaches(clip)) {
      const RenderScissor clipScissor = scissorForClip(sw, sh, bw, bh, clipLeft, clipTop, clipRight, clipBottom);
      if (m_passCount == 0) {
        m_backend->setScissor(clipScissor);
        drawNodeSelf(renderScale, node, worldTransform, effectiveOpacity, sw, sh, bw, bh, gridPhases);
        drawn = true;
      }
      for (std::size_t i = 0; i < m_passCount; ++i) {
        RepaintPass& pass = m_passes[i];
        if (pass.occluder != nullptr && !pass.occluderReached) {
          if (pass.occluder != node) {
            continue;
          }
          pass.occluderReached = true;
        }
        const RenderScissor scissor = intersectScissors(clipScissor, pass.scissor);
        if (!reaches(pass.logical) || scissor.width <= 0 || scissor.height <= 0) {
          continue;
        }
        m_backend->setScissor(scissor);
        drawNodeSelf(renderScale, node, worldTransform, effectiveOpacity, sw, sh, bw, bh, gridPhases);
        drawn = true;
      }
    }
    if ((gridPhases & kTerminalGridText) != 0) {
      ++(drawn ? m_statDrawn : m_statCulled);
    }
  };
  const auto drawChildren = [&](int minZ, int maxZ) {
    if (childrenClipped) {
      return;
    }
    forEachChildInPaintOrder(node, [&](const Node* child) {
      if (child->zIndex() < minZ || child->zIndex() > maxZ) {
        return;
      }
      renderNode(
          renderScale, child, worldTransform, effectiveOpacity, sw, sh, bw, bh, childClipLeft, childClipTop,
          childClipRight, childClipBottom, childHasClip, false, paintContained
      );
    });
  };

  constexpr int kMinZ = std::numeric_limits<int>::min();
  constexpr int kMaxZ = std::numeric_limits<int>::max();
  if (node->type() == NodeType::TerminalGrid) {
    drawSelf(kTerminalGridFill);
    drawChildren(kMinZ, TerminalGridNode::kBelowBackgroundZ);
    drawSelf(kTerminalGridBackgrounds);
    drawChildren(TerminalGridNode::kBelowBackgroundZ + 1, TerminalGridNode::kBelowTextZ);
    drawSelf(kTerminalGridText);
    drawChildren(TerminalGridNode::kBelowTextZ + 1, kMaxZ);
    return;
  }
  drawSelf(kTerminalGridAll);
  const std::optional<Radii>& roundClip = node->childClip();
  // Where no repaint pass reaches outside a corner arc, the scissor already gives the rounded clip: no stencil work.
  const auto passesReachCorners = [&] {
    const Mat3& m = worldTransform;
    if (m_passCount == 0 || m.m[1] != 0.0F || m.m[3] != 0.0F) {
      return true;
    }
    const float scale = std::min(std::abs(m.m[0]), std::abs(m.m[4]));
    const DamageRect box{boundsLeft, boundsTop, boundsRight, boundsBottom};
    return std::ranges::any_of(passes, [&](const RepaintPass& pass) {
      return reachesOutsideCorners(box, *roundClip, scale, 0.0F, pass.logical);
    });
  };
  const bool rounded = roundClip && !childrenClipped
                       && (roundClip->tl > 0.0F || roundClip->tr > 0.0F || roundClip->br > 0.0F || roundClip->bl > 0.0F)
                       && passesReachCorners();
  RenderScissor area{};
  if (rounded) {
    // Stencil work covers only where children can draw: the clip box within the repaint passes.
    area = scissorForClip(sw, sh, bw, bh, childClipLeft, childClipTop, childClipRight, childClipBottom);
    if (m_passCount > 0) {
      RenderScissor passes = m_passes[0].scissor;
      for (std::size_t i = 1; i < m_passCount; ++i) {
        const RenderScissor& s = m_passes[i].scissor;
        const std::int32_t x0 = std::min(passes.x, s.x);
        const std::int32_t y0 = std::min(passes.y, s.y);
        passes = {.x = x0, .y = y0, .width = std::max(passes.x + passes.width, s.x + s.width) - x0,
                  .height = std::max(passes.y + passes.height, s.y + s.height) - y0};
      }
      area = intersectScissors(area, passes);
    }
    m_backend->pushRoundedClip(sw, sh, node->width(), node->height(), *roundClip, worldTransform, area, ++m_roundedClipDepth);
  }
  drawChildren(kMinZ, kMaxZ);
  if (rounded) {
    m_backend->popRoundedClip(sw, sh, node->width(), node->height(), *roundClip, worldTransform, area, m_roundedClipDepth--);
  }
}

void RenderContext::drawNodeSelf(
    float renderScale, const Node* node, const Mat3& worldTransform, float effectiveOpacity, float sw, float sh, float bw,
    float bh, std::uint8_t gridPhases
) {
  switch (node->type()) {
  case NodeType::Rect: {
    const auto* rect = static_cast<const RectNode*>(node);
    auto style = rect->style();
    style.fill.a *= effectiveOpacity;
    style.border.a *= effectiveOpacity;
    for (auto& stop : style.gradientStops) {
      stop.color.a *= effectiveOpacity;
    }
    m_backend->drawRect(sw, sh, node->width(), node->height(), style, worldTransform);
    break;
  }
  case NodeType::Text: {
    const auto* text = static_cast<const TextNode*>(node);
    if (!text->text().empty()) {
      m_textRenderer.setCaching(!text->sensitive());
      const TextFont& font = text->font();
      if (text->hasShadow()) {
        auto shadowColor = text->shadowColor();
        shadowColor.a *= effectiveOpacity;
        const Mat3 shadowTransform = worldTransform * Mat3::translation(text->shadowOffsetX(), text->shadowOffsetY());
        m_textRenderer.draw(
            renderScale, sw, sh, 0.0F, 0.0F, text->text(), text->fontSize(), shadowColor, shadowTransform, font,
            text->maxWidth(), text->maxLines(), text->textAlign(), text->ellipsize(), text->useMarkup(),
            text->shadowBlur()
        );
      }
      auto color = text->color();
      color.a *= effectiveOpacity;
      m_textRenderer.draw(
          renderScale, sw, sh, 0.0F, 0.0F, text->text(), text->fontSize(), color, worldTransform, font,
          text->maxWidth(), text->maxLines(), text->textAlign(), text->ellipsize(), text->useMarkup()
      );
      m_textRenderer.setCaching(true);
    }
    break;
  }
  case NodeType::Image: {
    const auto* img = static_cast<const ImageNode*>(node);
    if (img->textureId() != 0) {
      // Rotated or skewed transforms keep their fractional origin; snapping them jumps whole pixels per frame.
      Mat3 imageTransform = worldTransform;
      if (isAxisAligned(imageTransform)) {
        imageTransform.m[6] = snapToBufferPixel(imageTransform.m[6], renderScale);
        imageTransform.m[7] = snapToBufferPixel(imageTransform.m[7], renderScale);
      }
      m_backend->drawImage(
          RenderImageDraw{
              .texture = img->textureId(),
              .surfaceWidth = sw,
              .surfaceHeight = sh,
              .width = node->width(),
              .height = node->height(),
              .tint = img->tint(),
              .monochromeTint = img->monochromeTint(),
              .alphaMaskTint = img->alphaMaskTint(),
              .opacity = effectiveOpacity,
              .radius = img->radius(),
              .borderColor = img->borderColor(),
              .borderWidth = img->borderWidth(),
              .fitMode = static_cast<RenderImageFitMode>(img->fitMode()),
              .textureWidth = static_cast<float>(img->textureWidth()),
              .textureHeight = static_cast<float>(img->textureHeight()),
              .transform = imageTransform,
              .scrim = img->scrim(),
              .uvRect = img->uvRect(),
          }
      );
    }
    break;
  }
  case NodeType::Glyph: {
    const auto* icon = static_cast<const GlyphNode*>(node);
    if (icon->codepoint() != 0) {
      if (icon->hasShadow()) {
        auto shadowColor = icon->shadowColor();
        shadowColor.a *= effectiveOpacity;
        const Mat3 shadowTransform = worldTransform * Mat3::translation(icon->shadowOffsetX(), icon->shadowOffsetY());
        m_glyphRenderer.drawGlyph(
            renderScale, sw, sh, 0.0F, 0.0F, icon->codepoint(), icon->fontSize(), shadowColor, shadowTransform,
            icon->shadowBlur()
        );
      }
      auto color = icon->color();
      color.a *= effectiveOpacity;
      m_glyphRenderer.drawGlyph(
          renderScale, sw, sh, 0.0F, 0.0F, icon->codepoint(), icon->fontSize(), color, worldTransform
      );
    }
    break;
  }
  case NodeType::Spinner: {
    const auto* spinner = static_cast<const SpinnerNode*>(node);
    auto style = spinner->style();
    style.color.a *= effectiveOpacity;
    m_backend->drawSpinner(sw, sh, node->width(), node->height(), style, worldTransform);
    break;
  }
  case NodeType::CountdownRing: {
    const auto* ring = static_cast<const CountdownRingNode*>(node);
    auto style = ring->style();
    style.color.a *= effectiveOpacity;
    m_backend->drawCountdownRing(sw, sh, node->width(), node->height(), style, worldTransform);
    break;
  }
  case NodeType::ScreenCorner: {
    const auto* corner = static_cast<const ScreenCornerNode*>(node);
    auto style = corner->style();
    style.color.a *= effectiveOpacity;
    m_backend->drawScreenCorner(sw, sh, node->width(), node->height(), style, worldTransform);
    break;
  }
  case NodeType::AudioSpectrum: {
    const auto* spectrum = static_cast<const AudioSpectrumNode*>(node);
    auto style = spectrum->style();
    style.color1.a *= effectiveOpacity;
    style.color2.a *= effectiveOpacity;
    const float pixelScaleX = sw > 0.0F ? bw / sw : 1.0F;
    const float pixelScaleY = sh > 0.0F ? bh / sh : 1.0F;
    m_backend->drawAudioSpectrum(
        sw, sh, pixelScaleX, pixelScaleY, node->width(), node->height(), style, spectrum->values(), worldTransform
    );
    break;
  }
  case NodeType::FancyAudioVisualizer: {
    const auto* visualizer = static_cast<const FancyAudioVisualizerNode*>(node);
    if (visualizer->textureId() != 0) {
      auto style = visualizer->style();
      style.primaryColor.a *= effectiveOpacity;
      style.secondaryColor.a *= effectiveOpacity;
      m_backend->drawFancyAudioVisualizer(
          visualizer->textureId(), visualizer->textureWidth(), sw, sh, node->width(), node->height(), style,
          worldTransform
      );
    }
    break;
  }
  case NodeType::Effect: {
    const auto* effect = static_cast<const EffectNode*>(node);
    auto style = effect->style();
    style.bgColor.a *= effectiveOpacity;
    m_backend->drawEffect(sw, sh, node->width(), node->height(), style, worldTransform);
    break;
  }
  case NodeType::Graph: {
    const auto* graph = static_cast<const GraphNode*>(node);
    if (graph->textureId() != 0) {
      auto style = graph->style();
      style.lineColor1.a *= effectiveOpacity;
      style.lineColor2.a *= effectiveOpacity;
      style.lineColor3.a *= effectiveOpacity;
      style.graphFillOpacity *= effectiveOpacity;
      m_backend->drawGraph(
          graph->textureId(), graph->textureWidth(), sw, sh, node->width(), node->height(), style, worldTransform
      );
    }
    break;
  }
  case NodeType::TerminalGrid: {
    const auto* grid = static_cast<const TerminalGridNode*>(node);
    TerminalGridDraw draw{
        .width = node->width(),
        .height = node->height(),
        .atlas = grid->atlasTexture(),
        .atlasWidth = grid->atlasWidth(),
        .atlasHeight = grid->atlasHeight(),
        .rects = grid->rectLists(),
        .glyphs = grid->glyphLists(),
        .over = grid->cursorRectLists(),
        .overGlyphs = grid->cursorGlyphLists(),
        .surfaceWidth = sw,
        .surfaceHeight = sh,
        .transform = worldTransform,
        .cellTransform = worldTransform * Mat3::translation(grid->cellOffset().x, grid->cellOffset().y),
        .opacity = effectiveOpacity,
        .phases = gridPhases,
    };
    const std::uint32_t bg = grid->background();
    const float bgAlpha = grid->backgroundAlpha();
    draw.background[0] = static_cast<float>((bg >> 16) & 0xFF) / 255.0F * bgAlpha;
    draw.background[1] = static_cast<float>((bg >> 8) & 0xFF) / 255.0F * bgAlpha;
    draw.background[2] = static_cast<float>(bg & 0xFF) / 255.0F * bgAlpha;
    draw.background[3] = bgAlpha;
    m_backend->drawTerminalGrid(draw);
    break;
  }
  case NodeType::ShaderScene: {
    const auto* scene = static_cast<const ShaderSceneNode*>(node);
    auto rgb = [](const Color& c) { return std::array<float, 3>{c.r, c.g, c.b}; };
    m_backend->drawShaderScene(ShaderSceneDraw{.source = scene->source(),
                                               .width = node->width(),
                                               .height = node->height(),
                                               .surfaceWidth = sw,
                                               .surfaceHeight = sh,
                                               .scale = renderScale,
                                               .time = scene->time(),
                                               .dark = scene->dark(),
                                               .surface = rgb(scene->surface()),
                                               .text = rgb(scene->text()),
                                               .accent = rgb(scene->accent()),
                                               .signals = scene->signals(),
                                               .secretDim = scene->secretDim(),
                                               .transform = worldTransform,
                                               .opacity = effectiveOpacity});
    break;
  }
  case NodeType::Wallpaper: {
    const auto* wallpaper = static_cast<const WallpaperNode*>(node);
    const bool hasSource1 = wallpaper->sourceKind1() == WallpaperSourceKind::Color || wallpaper->texture1() != 0;
    if (hasSource1) {
      const bool hasSource2 = wallpaper->sourceKind2() == WallpaperSourceKind::Color || wallpaper->texture2() != 0;
      const WallpaperSourceKind sourceKind2 = hasSource2 ? wallpaper->sourceKind2() : wallpaper->sourceKind1();
      const TextureId texture2 = hasSource2 ? wallpaper->texture2() : wallpaper->texture1();
      const Color& sourceColor2 = hasSource2 ? wallpaper->sourceColor2() : wallpaper->sourceColor1();
      const float imageWidth2 = hasSource2 ? wallpaper->imageWidth2() : wallpaper->imageWidth1();
      const float imageHeight2 = hasSource2 ? wallpaper->imageHeight2() : wallpaper->imageHeight1();
      const float progress = hasSource2 ? wallpaper->progress() : 0.0F;
      m_backend->drawWallpaper(
          WallpaperDrawParams{
              .transition = wallpaper->transition(),
              .from =
                  {.kind = wallpaper->sourceKind1(),
                   .texture = wallpaper->texture1(),
                   .color = wallpaper->sourceColor1(),
                   .imageWidth = wallpaper->imageWidth1(),
                   .imageHeight = wallpaper->imageHeight1()},
              .to =
                  {.kind = sourceKind2,
                   .texture = texture2,
                   .color = sourceColor2,
                   .imageWidth = imageWidth2,
                   .imageHeight = imageHeight2},
              .surfaceWidth = sw,
              .surfaceHeight = sh,
              .quadWidth = node->width(),
              .quadHeight = node->height(),
              .progress = progress,
              .fillMode = static_cast<float>(wallpaper->fillMode()),
              .params = wallpaper->transitionParams(),
              .fillColor = wallpaper->fillColor(),
              .transform = worldTransform,
              .span = wallpaper->spanParams(),
          }
      );
    }
    break;
  }
  case NodeType::TextureShader: {
    const auto* shader = static_cast<const TextureShaderNode*>(node);
    if (shader->texture() != 0) {
      m_backend->drawTextureShader(TextureShaderDraw{
          .fragment = shader->fragment(),
          .texture = shader->texture(),
          .uniforms = shader->uniforms(),
          .surfaceWidth = sw,
          .surfaceHeight = sh,
          .quadWidth = node->width(),
          .quadHeight = node->height(),
          .opacity = effectiveOpacity,
          .transform = worldTransform,
      });
    }
    break;
  }
  case NodeType::RenderProxy:
  case NodeType::Base:
    break;
  }
}

void RenderContext::cleanup() {
  if (m_backend != nullptr) {
    // Need a current context to destroy GL resources, but we may not have a surface.
    m_backend->makeCurrentNoSurface();
  }

  // Text renderers first — they destroy GL textures and need a current context.
  m_textRenderer.cleanup();
  m_glyphRenderer.cleanup();

  if (m_backend != nullptr) {
    m_backend->cleanup();
    m_backend.reset();
  }
  m_glAttach = {};
  m_glState = GlState::None;
}
