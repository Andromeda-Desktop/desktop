#include "wayland/surface.h"

#include "core/log.h"
#include "core/ui_phase.h"
#include "ext-background-effect-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wayland/frame_stats.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_egl_window.h"

#include <algorithm>
#include <format>
#include <limits>
#include <string>
#include <unordered_set>

namespace {

  constexpr Logger kLog("surface");

  std::unordered_set<const wl_surface*>& liveSurfaces() {
    static std::unordered_set<const wl_surface*> surfaces;
    return surfaces;
  }

  const wl_callback_listener kFrameListener = {
      .done = &WaylandSurface::handleFrameDone,
  };

  void surfaceEnter(void* data, wl_surface* surface, wl_output* output) {
    static_cast<WaylandSurface*>(data)->onSurfaceOutputEnter(surface, output);
  }

  void surfaceLeave(void* data, wl_surface* surface, wl_output* output) {
    static_cast<WaylandSurface*>(data)->onSurfaceOutputLeave(surface, output);
  }

  const wl_surface_listener kSurfaceListener = {
      .enter = surfaceEnter,
      .leave = surfaceLeave,
      .preferred_buffer_scale = nullptr,
      .preferred_buffer_transform = nullptr,
  };

  void preferredFractionalScale(void* data, wp_fractional_scale_v1* /*fractionalScale*/, std::uint32_t scale) {
    static_cast<WaylandSurface*>(data)->onPreferredFractionalScale(scale);
  }

  const wp_fractional_scale_v1_listener kFractionalScaleListener = {
      .preferred_scale = preferredFractionalScale,
  };

  InputRect boundsForRects(const std::vector<InputRect>& rects) {
    if (rects.empty()) {
      return {};
    }

    int minX = rects.front().x;
    int minY = rects.front().y;
    int maxX = rects.front().x + rects.front().width;
    int maxY = rects.front().y + rects.front().height;
    for (const auto& rect : rects) {
      minX = std::min(minX, rect.x);
      minY = std::min(minY, rect.y);
      maxX = std::max(maxX, rect.x + rect.width);
      maxY = std::max(maxY, rect.y + rect.height);
    }

    return InputRect{minX, minY, maxX - minX, maxY - minY};
  }

  std::string rectListPreview(const std::vector<InputRect>& rects) {
    if (rects.empty()) {
      return "[]";
    }

    constexpr std::size_t kPreviewCount = 6;
    const std::size_t previewCount = std::min(kPreviewCount, rects.size());
    std::string out = "[";
    for (std::size_t i = 0; i < previewCount; ++i) {
      const auto& rect = rects[i];
      if (i > 0) {
        out += ' ';
      }
      out += std::format("{}:{}+{}x{}", rect.x, rect.y, rect.width, rect.height);
    }
    if (rects.size() > previewCount) {
      out += std::format(" +{}", rects.size() - previewCount);
    }
    out += ']';
    return out;
  }

  std::string outputLabelForSurface(const WaylandConnection& connection, wl_surface* surface) {
    wl_output* wlOutput = connection.outputForSurface(surface);
    const WaylandOutput* output = connection.findOutputByWl(wlOutput);
    if (output == nullptr) {
      return "unknown";
    }
    if (!output->connectorName.empty()) {
      return output->connectorName;
    }
    return std::format("#{}", output->name);
  }

  wl_region* createRegion(WaylandConnection& connection, const std::vector<InputRect>& rects) {
    wl_region* region = wl_compositor_create_region(connection.compositor());
    if (region == nullptr) {
      return nullptr;
    }
    for (const auto& r : rects) {
      wl_region_add(region, r.x, r.y, r.width, r.height);
    }
    return region;
  }

} // namespace

WaylandSurface::WaylandSurface(WaylandConnection& connection) : m_connection(connection) {}

WaylandSurface::~WaylandSurface() { destroySurface(); }

void WaylandSurface::setOutputChangedCallback(OutputChangedCallback callback) {
  m_outputChangedCallback = std::move(callback);
}

void WaylandSurface::handleFrameDone(void* data, wl_callback* callback, std::uint32_t callbackData) {
  auto* self = static_cast<WaylandSurface*>(data);
  if (callback != nullptr) {
    wl_callback_destroy(callback);
  }
  self->m_frameCallback = nullptr;
  self->frameDone(callbackData);
}

void WaylandSurface::onSurfaceOutputEnter(wl_surface* surface, wl_output* output) {
  if (surface != m_surface || output == nullptr) {
    return;
  }
  m_connection.notifySurfaceOutputEnter(surface, output);
  if (m_outputChangedCallback) {
    m_outputChangedCallback(output);
  }

  const WaylandOutput* outputInfo = m_connection.findOutputByWl(output);
  if (outputInfo == nullptr) {
    return;
  }

  updateOutputScale(outputInfo->scale, static_cast<std::uint32_t>(std::max(1, outputInfo->configuredScaleNumerator)));
}

void WaylandSurface::onSurfaceOutputLeave(wl_surface* surface, wl_output* output) {
  if (surface != m_surface || output == nullptr) {
    return;
  }
  m_connection.notifySurfaceOutputLeave(surface, output);
  if (m_outputChangedCallback) {
    m_outputChangedCallback(m_connection.outputForSurface(m_surface));
  }
}

bool WaylandSurface::isLive(const wl_surface* surface) { return liveSurfaces().contains(surface); }

bool WaylandSurface::createWlSurface() {
  m_surface = wl_compositor_create_surface(m_connection.compositor());
  if (m_surface == nullptr) {
    return false;
  }
  liveSurfaces().insert(m_surface);
  trace("create-wl-surface");
  wl_surface_add_listener(m_surface, &kSurfaceListener, this);

  initializeSurfaceScaleProtocol();
  attachRenderTarget();
  return true;
}

std::unique_ptr<NativeEglWindow> WaylandSurface::createNativeWindow() {
  return std::make_unique<WaylandEglWindow>(m_surface);
}

void WaylandSurface::requestFrameCallback() {
  m_frameCallback = wl_surface_frame(m_surface);
  if (m_frameCallback != nullptr) {
    wl_callback_add_listener(m_frameCallback, &kFrameListener, this);
  }
}

void WaylandSurface::cancelFrameCallback() {
  if (m_frameCallback != nullptr) {
    wl_callback_destroy(m_frameCallback);
    m_frameCallback = nullptr;
  }
}

void WaylandSurface::commitNative() { wl_surface_commit(m_surface); }

bool WaylandSurface::scalesByViewport() const noexcept { return m_fractionalScale != nullptr && m_viewport != nullptr; }

void WaylandSurface::initializeSurfaceScaleProtocol() {
  if (m_surface == nullptr || !m_connection.hasFractionalScale()) {
    return;
  }

  auto* viewporter = m_connection.viewporter();
  auto* fractionalScaleManager = m_connection.fractionalScaleManager();
  if (viewporter == nullptr || fractionalScaleManager == nullptr) {
    return;
  }

  m_viewport = wp_viewporter_get_viewport(viewporter, m_surface);
  m_fractionalScale = wp_fractional_scale_manager_v1_get_fractional_scale(fractionalScaleManager, m_surface);
  if (m_fractionalScale != nullptr) {
    wp_fractional_scale_v1_add_listener(m_fractionalScale, &kFractionalScaleListener, this);
  }

  // Fractional-scale-v1 requires wl_surface buffer scale to remain 1; viewport
  // destination keeps the surface-local size equal to the role configure size.
  wl_surface_set_buffer_scale(m_surface, 1);
}

void WaylandSurface::applyNativeScale() {
  if (m_surface == nullptr) {
    return;
  }

  if (scalesByViewport()) {
    wl_surface_set_buffer_scale(m_surface, 1);
    if (width() > 0 && height() > 0) {
      constexpr auto kMaxViewportExtent = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
      if (width() > kMaxViewportExtent || height() > kMaxViewportExtent) {
        kLog.warn("skipping viewport destination with out-of-range size {}x{}", width(), height());
      } else {
        wp_viewport_set_destination(m_viewport, static_cast<std::int32_t>(width()), static_cast<std::int32_t>(height()));
      }
    }
    return;
  }

  wl_surface_set_buffer_scale(m_surface, std::max(1, bufferScale()));
}

void WaylandSurface::onPreferredFractionalScale(std::uint32_t numerator) {
  if (numerator == 0 || numerator == preferredScaleNumerator()) {
    return;
  }
  kLog.debug(
      "fractional scale preferred output={} surface={} scale={:.3F} raw={}/120 logical={}x{}",
      outputLabelForSurface(m_connection, m_surface), static_cast<const void*>(m_surface),
      std::max(1.0F, static_cast<float>(numerator) / 120.0F), numerator, width(), height()
  );
  setPreferredScaleNumerator(numerator);
}

void WaylandSurface::setInputRegion(const std::vector<InputRect>& rects) {
  if (m_surface == nullptr) {
    return;
  }
  wl_region* region = createRegion(m_connection, rects);
  if (region == nullptr) {
    return;
  }
  wl_surface_set_input_region(m_surface, region);
  wl_region_destroy(region);
}

void WaylandSurface::setOpaqueRegion(const std::vector<InputRect>& rects) {
  if (m_surface == nullptr) {
    return;
  }
  wl_region* region = createRegion(m_connection, rects);
  if (region == nullptr) {
    return;
  }
  wl_surface_set_opaque_region(m_surface, region);
  wl_region_destroy(region);
  m_opaque = false; // not the whole surface: a later setOpaque(true) must still apply
}

void WaylandSurface::setOpaque(bool opaque) {
  if (m_surface == nullptr || m_opaque == opaque) {
    return;
  }
  if (!opaque) {
    wl_surface_set_opaque_region(m_surface, nullptr);
    m_opaque = false;
    return;
  }
  // The compositor clips the region to the surface, so it stays valid across resizes.
  wl_region* region = createRegion(
      m_connection, {{0, 0, std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::max()}}
  );
  if (region == nullptr) {
    return;
  }
  wl_surface_set_opaque_region(m_surface, region);
  wl_region_destroy(region);
  m_opaque = true;
}

bool WaylandSurface::prepareBlurEffect() {
  if (m_surface == nullptr) {
    trace("blur-effect-skip-no-surface");
    return false;
  }
  if (!m_connection.hasBackgroundEffectBlur()) {
    trace("blur-effect-skip-no-protocol");
    return false;
  }
  if (m_backgroundEffect != nullptr) {
    return true;
  }

  auto* manager = m_connection.backgroundEffectManager();
  if (manager == nullptr) {
    trace("blur-effect-skip-no-manager");
    return false;
  }
  m_backgroundEffect = ext_background_effect_manager_v1_get_background_effect(manager, m_surface);
  if (m_backgroundEffect == nullptr) {
    trace("blur-effect-skip-create-failed");
    return false;
  }
  trace("blur-effect-create");
  return true;
}

void WaylandSurface::traceBlurRegion(std::string_view event, const std::vector<InputRect>& rects) const {
  if (!traceEnabled()) {
    return;
  }

  const InputRect bounds = boundsForRects(rects);
  const int right = bounds.x + bounds.width;
  const int bottom = bounds.y + bounds.height;
  const bool fullSurface = !rects.empty()
      && width() > 0
      && height() > 0
      && bounds.x <= 0
      && bounds.y <= 0
      && right >= static_cast<int>(width())
      && bottom >= static_cast<int>(height());
  kLog.debug(
      "blur-trace {} name={} self={} phase={} logical={}x{} scale={:.3F} rects={} bounds={}:{}+{}x{} "
      "full_surface={} sample={}",
      event, debugName().empty() ? std::string_view("surface") : std::string_view(debugName()),
      static_cast<const void*>(this), uiPhaseName(currentUiPhase()), width(), height(), effectiveBufferScale(),
      rects.size(), bounds.x, bounds.y, bounds.width, bounds.height, fullSurface, rectListPreview(rects)
  );
}

void WaylandSurface::setBlurRegion(const std::vector<InputRect>& rects) {
  if (!prepareBlurEffect()) {
    return;
  }

  // Hyprland renders a fully off-surface non-empty blur region as full-surface blur, so send null instead.
  const bool hasVisibleRegion = regionIntersectsBounds(rects, width(), height());
  wl_region* region = nullptr;
  if (hasVisibleRegion) {
    region = createRegion(m_connection, rects);
    if (region == nullptr) {
      trace("blur-set-skip-region-failed");
      return;
    }
  }
  const char* traceLabel = "blur-set";
  if (!hasVisibleRegion) {
    traceLabel = rects.empty() ? "blur-set-empty" : "blur-set-offsurface";
  }
  traceBlurRegion(traceLabel, rects);
  ext_background_effect_surface_v1_set_blur_region(m_backgroundEffect, region);
  if (region != nullptr) {
    wl_region_destroy(region);
  }
}

void WaylandSurface::clearBlurRegion() {
  if (m_backgroundEffect == nullptr) {
    trace("blur-clear-no-effect");
    return;
  }
  trace("blur-clear-destroy");
  ext_background_effect_surface_v1_destroy(m_backgroundEffect);
  m_backgroundEffect = nullptr;
}

FrameStatsSink* WaylandSurface::frameStats(bool create) {
  if (create
      && m_frameStats == nullptr
      && m_surface != nullptr
      && FrameStats::enabled()
      && m_connection.presentation() != nullptr) {
    m_frameStats = std::make_unique<FrameStats>(
        m_connection.presentation(), m_surface, debugName().empty() ? std::string("surface") : debugName()
    );
  }
  return m_frameStats.get();
}

void WaylandSurface::destroySurface() {
  m_frameStats.reset();
  cancelFrameCallback();
  releaseRenderState();

  if (m_backgroundEffect != nullptr) {
    ext_background_effect_surface_v1_destroy(m_backgroundEffect);
    m_backgroundEffect = nullptr;
  }

  if (m_fractionalScale != nullptr) {
    wp_fractional_scale_v1_destroy(m_fractionalScale);
    m_fractionalScale = nullptr;
  }

  if (m_viewport != nullptr) {
    wp_viewport_destroy(m_viewport);
    m_viewport = nullptr;
  }

  if (m_surface != nullptr) {
    liveSurfaces().erase(m_surface);
    wl_surface_destroy(m_surface);
    m_surface = nullptr;
    m_opaque = false;
  }
}
