#include "shell/osd/toast_overlay.h"

#include "cli/schema_msg.h"
#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/ui_phase.h"
#include "ipc/ipc_service.h"
#include "render/core/renderer.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

  using Property = DskSurfaceClient::Property;
  constexpr double kScaleFrom = 0.92;

  [[nodiscard]] DskSurfaceClient::Value scalar(double v) { return {v, 0.0, 0.0, 0.0}; }

} // namespace

void ToastOverlay::initialize(
    WaylandConnection& wayland, CompositorPlatform& platform, ConfigService* config, RenderContext* renderContext
) {
  m_wayland = &wayland;
  m_platform = &platform;
  m_config = config;
  m_renderContext = renderContext;
}

void ToastOverlay::registerIpc(IpcService& ipc) {
  ipc.bind(noctalia::cli::msg::toastShow, [this](const std::string& args) -> std::string {
    std::string text = StringUtils::trim(args);
    if (text.empty()) {
      return "error: toast-show requires <text>\n";
    }
    show(std::move(text));
    return "ok\n";
  });
}

float ToastOverlay::uiScale() const {
  return m_config != nullptr ? std::max(0.1F, m_config->config().accessibility.uiScale) : 1.0F;
}

void ToastOverlay::show(std::string text) {
  if (m_wayland == nullptr || m_renderContext == nullptr) {
    return;
  }
  m_text = std::move(text);
  m_textChanged = true;
  switch (m_phase) {
  case Phase::Hidden:
    createSurface();
    if (m_surface == nullptr) {
      return;
    }
    break;
  case Phase::Hiding:
    // The fade-out retargets to 1; the compositor keeps the current opacity as the start.
    m_glass.animate(Property::Opacity, DskSurfaceClient::Curve::SpringOpen, scalar(1.0));
    break;
  case Phase::Shown:
    break;
  }
  m_phase = Phase::Shown;
  m_surface->requestLayout();
  m_timeout.start(std::chrono::milliseconds(Tokens::componentToastTimeoutMs), [this] { startHide(); });
}

void ToastOverlay::startHide() {
  if (m_phase != Phase::Shown) {
    return;
  }
  m_phase = Phase::Hiding;
  const bool animated = m_glass.animate(Property::Opacity, DskSurfaceClient::Curve::SpringEffectsFast, scalar(0.0), [this](auto reason) {
    if (reason != DskSurfaceClient::EndReason::Finished) {
      return;
    }
    DeferredCall::callLaterIfAlive(m_alive, [this] {
      if (m_phase == Phase::Hiding) {
        destroySurface();
      }
    });
  });
  if (!animated) {
    destroySurface();
  }
}

void ToastOverlay::createSurface() {
  wl_output* output = m_platform != nullptr ? m_platform->preferredInteractiveOutput() : nullptr;
  if (output == nullptr) {
    for (const auto& o : m_wayland->outputs()) {
      if (o.done && o.output != nullptr && o.hasUsableGeometry()) {
        output = o.output;
        break;
      }
    }
  }
  if (output == nullptr) {
    return;
  }
  const float s = uiScale();
  const auto height = static_cast<std::uint32_t>(std::ceil(Tokens::componentToastHeight * s));
  auto config = LayerSurfaceConfig{
      .nameSpace = "noctalia-toast",
      .layer = LayerShellLayer::Overlay,
      .anchor = LayerShellAnchor::Bottom | LayerShellAnchor::Left | LayerShellAnchor::Right,
      .width = 0,
      .height = height,
      // -1: the margin counts from the output edge, not from the dock's exclusive zone.
      .exclusiveZone = -1,
      .marginBottom = static_cast<std::int32_t>(std::round(Tokens::componentToastBottom * s)),
      .keyboard = LayerShellKeyboard::None,
      .defaultHeight = height,
      .prewarmBlur = true,
  };
  m_surface = std::make_unique<LayerSurface>(*m_wayland, std::move(config));
  m_surface->setRenderContext(m_renderContext);
  m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) { m_surface->requestLayout(); });
  m_surface->setPrepareFrameCallback([this](bool, bool) { prepareFrame(); });
  // The compositor closes the toast with its output; the next show() must create a new one.
  m_surface->setClosedCallback([this] { destroySurface(); });
  m_surface->setAnimationManager(&m_animations);
  if (!m_surface->initialize(output)) {
    m_surface.reset();
    return;
  }
  m_surface->setInputRegion({});
}

void ToastOverlay::destroySurface() {
  m_timeout.stop();
  m_animations.cancelAll();
  m_glassConn = {};
  m_root.reset();
  m_capsule = nullptr;
  m_label = nullptr;
  m_glyph = nullptr;
  m_glass.reset();
  m_surface.reset();
  m_phase = Phase::Hidden;
}

void ToastOverlay::prepareFrame() {
  if (m_surface == nullptr || m_renderContext == nullptr) {
    return;
  }
  const auto width = static_cast<float>(m_surface->width());
  const auto height = static_cast<float>(m_surface->height());
  if (width <= 0.0F || height <= 0.0F) {
    return;
  }
  m_renderContext->makeCurrent(m_surface->renderTarget());
  Renderer& renderer = m_surface->renderTarget().renderer();
  UiPhaseScope layoutPhase(UiPhase::Layout);
  const float s = uiScale();
  const bool first = m_root == nullptr;
  if (first) {
    m_root = ui::node({});
    Flex* row = nullptr;
    m_root->addChild(ui::row({
        .out = &row,
        .align = FlexAlign::Center,
        .gap = Tokens::spaceP8 * s,
        .fill = colorSpecFromRole(materialRole(), Style::islandGlass().tint),
        .radius = height * 0.5F,
        .height = height,
    }));
    row->setPadding(0.0F, Tokens::spaceP16 * s, 0.0F, Tokens::spaceP12 * s);
    m_glassConn = bindIslandTone(m_root, [this] { m_surface->requestLayout(); });
    row->addChild(ui::glyph({
        .out = &m_glyph,
        .glyph = std::string("check"),
        .glyphSize = Tokens::sizeSymbol * s,
        .color = colorSpecFromRole(ColorRole::Label1),
    }));
    row->addChild(ui::label({
        .out = &m_label,
        .fontSize = Tokens::typeBody.size * s,
        .color = colorSpecFromRole(ColorRole::Label1),
        .maxWidth = width - 2.0F * Tokens::spaceP16 * s - Tokens::sizeSymbol * s,
        .maxLines = 1,
    }));
    m_capsule = row;
    m_surface->setSceneRoot(m_root.get());
  }
  m_root->setSize(width, height);
  if (m_textChanged) {
    m_label->setText(m_text);
    m_textChanged = false;
  }
  auto* row = static_cast<Flex*>(m_capsule);
  row->layout(renderer);
  const float x = std::round((width - row->width()) * 0.5F);
  row->setPosition(x, 0.0F);
  const glass::Material material = Style::islandGlass();
  const bool compositorTint = m_surface->setGlassCard(GlassCard{
      .rect = GlassRect::from({static_cast<int>(x), 0, static_cast<int>(std::ceil(row->width())), static_cast<int>(height)}),
      .radius = height * 0.5F,
      .material = material,
      .node = row,
  });
  row->setFill(compositorTint ? clearColorSpec() : colorSpecFromRole(materialRole(), material.tint));
  m_glass.apply(*m_surface, GlassSurface::Kind::Shape);
  if (first) {
    m_glass.set(Property::Scale, scalar(kScaleFrom));
    m_glass.set(Property::Opacity, scalar(0.0));
    m_glass.animate(Property::Scale, DskSurfaceClient::Curve::SpringOpen, scalar(1.0));
    m_glass.animate(Property::Opacity, DskSurfaceClient::Curve::SpringOpen, scalar(1.0));
  }
}
