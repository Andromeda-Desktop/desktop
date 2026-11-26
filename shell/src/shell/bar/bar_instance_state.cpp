#include "shell/bar/bar.h"

#include "shell/bar/bar_internal.h"
#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/process/process.h"
#include "core/scoped_timer.h"
#include "core/timer_manager.h"
#include "core/ui_phase.h"
#include "idle/idle_inhibitor.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "shell/bar/bar_corner_shape.h"
#include "shell/bar/bar_reserved_zone.h"
#include "shell/bar/widget.h"
#include "shell/bar/widget_gesture_defaults.h"
#include "shell/bar/widgets/plugin_widget.h"
#include "shell/bar/widgets/taskbar_widget.h"
#include "shell/bar/widgets/spacer_widget.h"
#include "shell/bar/widgets/tray_widget.h"
#include "shell/panel/panel_manager.h"
#include "render/animation/motion_service.h"
#include "wayland/dsk_surface_client.h"
#include "wayland/glass_surface.h"
#include "ui/surface_shadow.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <linux/input-event-codes.h>
#include <optional>
#include <ranges>
#include <unordered_set>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

using namespace bar_detail;

namespace {

  constexpr Logger kLog("bar");

} // namespace

namespace {

  constexpr std::int32_t kAutoHideTriggerPx = 3;

  [[nodiscard]] std::vector<InputRect>
  barAutoHideSurfaceInputRegion(const BarConfig& cfg, int surfW, int surfH, bool fullSurface) {
    if (surfW <= 0 || surfH <= 0) {
      return {};
    }
    if (fullSurface) {
      return {InputRect{0, 0, surfW, surfH}};
    }

    const int strip = std::min(kAutoHideTriggerPx, cfg.position == "left" || cfg.position == "right" ? surfW : surfH);
    if (cfg.position == "bottom") {
      return {InputRect{0, surfH - strip, surfW, strip}};
    }
    if (cfg.position == "left") {
      return {InputRect{0, 0, strip, surfH}};
    }
    if (cfg.position == "right") {
      return {InputRect{surfW - strip, 0, strip, surfH}};
    }
    return {InputRect{0, 0, surfW, strip}};
  }


  std::uint32_t positionToAnchor(const std::string& position) {
    if (position == "bottom") {
      return LayerShellAnchor::Bottom | LayerShellAnchor::Left | LayerShellAnchor::Right;
    }
    if (position == "left") {
      return LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left;
    }
    if (position == "right") {
      return LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Right;
    }
    // Default: top
    return LayerShellAnchor::Top | LayerShellAnchor::Left | LayerShellAnchor::Right;
  }


  [[nodiscard]] InputRect
  barContentInputRegion(const BarConfig& cfg, const ShellConfig::ShadowConfig& shadow, int surfW, int surfH) {
    const float innerSurfaceExtension =
        barInnerSurfaceExtension(cfg, shadow, static_cast<float>(surfW), static_cast<float>(surfH));
    const auto barVisual = computeBarVisualGeometry(
        cfg, shadow, static_cast<float>(surfW), static_cast<float>(surfH), innerSurfaceExtension
    );
    return InputRect{
        static_cast<int>(barVisual.x), static_cast<int>(barVisual.y), static_cast<int>(barVisual.width),
        static_cast<int>(barVisual.height)
    };
  }

  // Click-through dead zone: one rect per interactive widget, spanning the bar's thickness and padded
  // along the bar by the capsule padding that widens widget hit targets past their bounds. Spacers and
  // passive widgets are dead zone (see widgetAtPoint), so they pass through too.
  [[nodiscard]] std::vector<InputRect> barWidgetsInputRegion(const BarInstance& instance, const InputRect& bar) {
    const bool isVertical = (instance.barConfig.position == "left" || instance.barConfig.position == "right");
    const float pad = instance.barConfig.widgetCapsulePadding;
    std::vector<InputRect> rects;
    for (const auto* widgets : {&instance.startWidgets, &instance.centerWidgets, &instance.endWidgets}) {
      for (const auto& widget : *widgets) {
        const Node* node = widget != nullptr ? widget->outerNode() : nullptr;
        if (node == nullptr
            || widget->isBarClickThrough()
            || !node->visible()
            || node->width() <= 0.0F
            || node->height() <= 0.0F) {
          continue;
        }
        float x = 0.0F;
        float y = 0.0F;
        Node::absolutePosition(node, x, y);
        if (isVertical) {
          const int top = std::max(bar.y, static_cast<int>(std::floor(y - pad)));
          const int bottom = std::min(bar.y + bar.height, static_cast<int>(std::ceil(y + node->height() + pad)));
          if (bottom > top) {
            rects.push_back(InputRect{bar.x, top, bar.width, bottom - top});
          }
        } else {
          const int left = std::max(bar.x, static_cast<int>(std::floor(x - pad)));
          const int right = std::min(bar.x + bar.width, static_cast<int>(std::ceil(x + node->width() + pad)));
          if (right > left) {
            rects.push_back(InputRect{left, bar.y, right - left, bar.height});
          }
        }
      }
    }
    return rects;
  }


} // namespace

void Bar::syncInstances() {
  const auto& outputs = m_platform->outputs();
  const auto& bars = m_config->config().bars;

  const auto geometryChanged = [](const BarInstance& instance, const WaylandOutput& output) {
    return instance.outputLogicalX != output.logicalX
        || instance.outputLogicalY != output.logicalY
        || instance.outputLogicalWidth != output.effectiveLogicalWidth()
        || instance.outputLogicalHeight != output.effectiveLogicalHeight();
  };

  std::erase_if(m_lastActiveWorkspaceByOutput, [&outputs](const auto& pair) {
    return std::ranges::find(outputs, pair.first, &WaylandOutput::name) == outputs.end();
  });

  for (const auto& output : outputs) {
    if (!output.done || !output.hasUsableGeometry()) {
      continue;
    }
    auto& last = m_lastActiveWorkspaceByOutput[output.name];
    if (last.empty()) {
      last = activeWorkspaceId(m_platform->workspaces(output.output));
    }
  }

  // Layer-shell surfaces are output-bound, but some compositors do not reposition an existing surface after that
  // output moves. Recreate every bar on an affected output to preserve their config-order stacking.
  std::unordered_set<std::uint32_t> outputsNeedingRecreate;
  for (const auto& instance : m_instances) {
    const auto output = std::ranges::find(outputs, instance->outputName, &WaylandOutput::name);
    if (output != outputs.end() && output->done && output->hasUsableGeometry() && geometryChanged(*instance, *output)) {
      outputsNeedingRecreate.insert(output->name);
    }
  }

  // Remove instances for outputs that no longer exist or whose output geometry changed.
  std::erase_if(m_instances, [&outputs, &outputsNeedingRecreate, this](const auto& inst) {
    const auto it = std::ranges::find(outputs, inst->outputName, &WaylandOutput::name);
    const bool found = it != outputs.end() && it->done && it->hasUsableGeometry();
    const bool recreate = found && outputsNeedingRecreate.contains(inst->outputName);
    if (!found || recreate) {
      kLog.info("removing instance for output {}", inst->outputName);
    }
    if (!found || recreate) {
      if (inst->surface != nullptr) {
        m_surfaceMap.erase(inst->surface.get());
      }
      if (m_hoveredInstance == inst.get()) {
        m_hoveredInstance = nullptr;
      }
    }
    return !found || recreate;
  });
  // Before any create: a new surface can reuse a destroyed one's address, and the inhibitors are keyed by it.
  syncIdleInhibitorAnchors();
  if (m_centerIslandSpaceCallback) {
    m_centerIslandSpaceCallback();
  }

  // Create instances for each bar definition × each output
  for (std::size_t barIdx = 0; barIdx < bars.size(); ++barIdx) {
    for (const auto& output : outputs) {
      if (!output.done || !output.hasUsableGeometry()) {
        continue;
      }

      bool exists = std::ranges::any_of(m_instances, [&output, barIdx](const auto& inst) {
        return inst->outputName == output.name && inst->barIndex == barIdx;
      });
      if (!exists) {
        auto resolved = resolveBar(bars[barIdx], output);
        if (!resolved.enabled) {
          continue;
        }
        createInstance(output, barIdx, resolved);
      }
    }
  }

  syncIdleInhibitorAnchors();
  reevaluateSmartAutoHide();
}

void Bar::createInstance(const WaylandOutput& output, std::size_t barIndex, const BarConfig& barConfig) {
  auto instance = std::make_unique<BarInstance>();
  instance->outputName = output.name;
  instance->output = output.output;
  instance->scale = output.scale;
  instance->outputLogicalX = output.logicalX;
  instance->outputLogicalY = output.logicalY;
  instance->outputLogicalWidth = output.effectiveLogicalWidth();
  instance->outputLogicalHeight = output.effectiveLogicalHeight();
  instance->barConfig = barConfig;
  instance->barIndex = barIndex;

  const auto anchor = positionToAnchor(barConfig.position);
  const auto surfaceSpec = computeBarSurfaceSpec(barConfig, m_config->config().shell.shadow);

  kLog.info(
      "creating #{} \"{}\" on {} ({}), thickness={} position={} reserve_space={} exclusive_zone={}", barIndex,
      barConfig.name, output.connectorName, output.description, barConfig.thickness, barConfig.position,
      barConfig.reserveSpace, surfaceSpec.exclusiveZone
  );

  auto surfaceConfig = LayerSurfaceConfig{
      .nameSpace = "noctalia-bar-" + barConfig.name,
      .layer = restingBarLayer(barConfig),
      .anchor = anchor,
      .width = surfaceSpec.surfaceWidth,
      .height = surfaceSpec.surfaceHeight,
      .exclusiveZone = surfaceSpec.exclusiveZone,
      .marginTop = surfaceSpec.marginTop,
      .marginRight = surfaceSpec.marginRight,
      .marginBottom = surfaceSpec.marginBottom,
      .marginLeft = surfaceSpec.marginLeft,
      .defaultHeight = surfaceSpec.surfaceHeight,
      .prewarmBlur = barConfig.compositorBlur,
  };

  instance->surface = std::make_unique<LayerSurface>(m_platform->wayland(), std::move(surfaceConfig));
  instance->surface->setRenderContext(m_renderContext);

  auto* inst = instance.get();
  instance->surface->setConfigureCallback([this, inst](std::uint32_t width, std::uint32_t height) {
    buildScene(*inst, width, height);
  });
  instance->surface->setPrepareFrameCallback([this, inst](bool needsUpdate, bool needsLayout) {
    prepareFrame(*inst, needsUpdate, needsLayout);
  });
  instance->surface->setFrameTickCallback([this, inst](float deltaMs) {
    tickWidgets(inst->startWidgets, deltaMs);
    tickWidgets(inst->centerWidgets, deltaMs);
    tickWidgets(inst->endWidgets, deltaMs);
    if (inst->autoLayer == BarInstance::AutoLayer::Settling) {
      // Frame work runs before this frame renders; the deferred call runs after it, so the final state is painted.
      DeferredCall::callLater([this, output = inst->output, barName = inst->barConfig.name] {
        BarInstance* instance = instanceForBar(output, barName);
        if (instance != nullptr && instance->surface != nullptr
            && instance->autoLayer == BarInstance::AutoLayer::Settling) {
          instance->autoLayer = BarInstance::AutoLayer::Resting;
          instance->surface->setLayer(effectiveBarLayer(*instance));
        }
      });
    }
  });

  instance->surface->setPresenceChangedCallback([inst](SurfacePresence presence) {
    for (const auto* widgets : {&inst->startWidgets, &inst->centerWidgets, &inst->endWidgets}) {
      for (const auto& widget : *widgets) {
        if (auto* plugin = dynamic_cast<PluginWidget*>(widget.get()); plugin != nullptr) {
          plugin->setShown(presence == SurfacePresence::Shown);
        }
      }
    }
  });
  instance->surface->setAnimationManager(&instance->animations);
  populateWidgets(*instance);

  if (!instance->surface->initialize(output.output)) {
    kLog.warn("failed to initialize surface for output {}", output.name);
    return;
  }

  m_surfaceMap[instance->surface.get()] = instance.get();
  m_instances.push_back(std::move(instance));
  if (m_centerIslandSpaceCallback) {
    m_centerIslandSpaceCallback();
  }
}

bool Bar::syncIslandLiquid(BarInstance& instance) {
  dsk_surface_manager_v1* manager = m_platform != nullptr ? m_platform->wayland().dskSurfaceManager() : nullptr;
  const bool want = instance.barConfig.islands && !Style::reduceTransparency();
  if (instance.dsk == nullptr && want && manager != nullptr && instance.surface != nullptr
      && instance.surface->wlSurface() != nullptr) {
    instance.dsk = instance.surface->dsk();
    instance.dsk->setOnDensity([inst = &instance](float density) {
      if (inst->sceneRoot != nullptr) {
        inst->sceneRoot->setInkLift(std::round(density * 32.0F) / 32.0F);
      }
    });
  }
  instance.compositorTint = false;
  if (instance.dsk == nullptr || !instance.dsk->hasLiquidProperty()) {
    return false;
  }
  sendIslandGlass(*instance.dsk, *instance.dsk, want);
  if (instance.dsk->hasTintProperty()) {
    instance.compositorTint = want && instance.barConfig.compositorBlur && Style::compositorDrawsTint();
    instance.dsk->set(
        DskSurfaceClient::Property::Tint, islandGlassTint(instance.compositorTint, instance.sceneRoot.get())
    );
  }
  syncIslandShapes(instance, true);
  return want;
}

// An island bar draws only inside its islands: a declared content region lets glass near the bar (Control Center
// pieces) sample the cached backdrop instead of blurring live in every frame. The pad keeps strokes and badges inside.
void Bar::syncContentRegion(BarInstance& instance) const {
  if (instance.dsk == nullptr || m_platform == nullptr) {
    return;
  }
  std::optional<std::vector<DskSurfaceClient::Value>> rects;
  if (instance.barConfig.islands && !instance.islandShapes.empty()) {
    constexpr double kPad = 8.0;
    rects.emplace();
    for (const auto& clip : instance.islandShapeClips) {
      if (clip[2] > 0.0 && clip[3] > 0.0) {
        rects->push_back({clip[0] - kPad, clip[1] - kPad, clip[2] + 2 * kPad, clip[3] + 2 * kPad});
      }
    }
  }
  if (rects == instance.contentRects) {
    return;
  }
  wl_region* region = nullptr;
  if (rects.has_value()) {
    region = wl_compositor_create_region(m_platform->wayland().compositor());
    for (const auto& r : *rects) {
      wl_region_add(region, static_cast<int>(r[0]), static_cast<int>(r[1]), static_cast<int>(r[2]), static_cast<int>(r[3]));
    }
  }
  if (instance.dsk->setContentRegion(region)) {
    instance.contentRects = std::move(rects);
  }
  if (region != nullptr) {
    wl_region_destroy(region);
  }
}

bool Bar::syncIslandShapes(BarInstance& instance, bool material) const {
  if (instance.dsk == nullptr || !instance.dsk->hasShapes() || !instance.barConfig.compositorBlur) {
    syncContentRegion(instance);
    return false;
  }
  if (!instance.islandBlurStrips.empty() && instance.surface != nullptr) {
    instance.surface->clearBlurRegion();
    instance.islandBlurStrips.clear();
  }
  const bool shown = barContentVisuallyShown(instance);
  while (instance.islandShapes.size() < instance.islands.size()) {
    instance.islandShapes.push_back(std::make_unique<DskShapeClient>(*instance.dsk));
    instance.islandShapeClips.push_back({});
    material = true;
  }
  for (std::size_t i = 0; i < instance.islandShapes.size(); ++i) {
    DskShapeClient& shape = *instance.islandShapes[i];
    const Box* island = i < instance.islands.size() ? instance.islands[i] : nullptr;
    DskSurfaceClient::Value clip{};
    if (shown && island != nullptr && island->visible() && island->width() >= 1.0F && island->height() >= 1.0F) {
      float x = 0.0F;
      float y = 0.0F;
      Node::absolutePosition(island, x, y);
      // The top-left is already scaled (a pressed island scales about its centre); the size is not. Unrounded: the
      // compositor draws the glass edge at sub-pixel precision, so a press shrinks it smoothly.
      clip = {x, y, island->width() * island->scaleX(), island->height() * island->scaleY()};
    }
    // A moved island resends only its own material: the others did not change.
    bool shapeMaterial = material;
    if (clip != instance.islandShapeClips[i]) {
      shapeMaterial = true;
      shape.set(DskSurfaceClient::Property::Clip, clip);
      shape.set(DskSurfaceClient::Property::Radius, {std::min(clip[2], clip[3]) * 0.5, 0.0, 0.0, 0.0});
      instance.islandShapeClips[i] = clip;
    }
    if (shapeMaterial) {
      // Level 1 = the float plane (tokens elevation.e2), the island shadow of the rulebook.
      shape.set(
          DskSurfaceClient::Property::Elevation,
          {instance.islandShadowStyle.has_value() ? 1.0 : 0.0, 0.0, 0.0, 0.0}
      );
      sendIslandGlass(shape, *instance.dsk, true);
      shape.set(DskSurfaceClient::Property::Tint, islandGlassTint(instance.compositorTint, instance.sceneRoot.get()));
    }
  }
  syncContentRegion(instance);
  return true;
}

void Bar::applyBackgroundPalette(BarInstance& instance) {
  if (instance.bg == nullptr) {
    return;
  }
  const Node* tone = instance.sceneRoot.get();
  if (instance.sceneRoot != nullptr) {
    applyIslandTone(*instance.sceneRoot);
  }
  const Color material = colorForRole(materialRole(), barBodyTint(instance.barConfig), tone);
  // Islands are small shapes: liquid glass with the launcher pill's clear tint.
  const bool liquid = syncIslandLiquid(instance);
  const Color islandMaterial = instance.compositorTint ? rgba(0.0F, 0.0F, 0.0F, 0.0F)
      : liquid                                        ? colorForRole(materialRole(), Style::islandGlass().tint, tone)
                                                      : material;
  const auto applyMaterial = [&](Box* box, bool painted) {
    auto style = box->style();
    style.fill = painted ? (box == instance.bg ? material : islandMaterial) : rgba(0.0F, 0.0F, 0.0F, 0.0F);
    style.border = resolveColorSpec(instance.barConfig.border, tone);
    style.borderWidth = painted ? instance.barConfig.borderWidth : 0.0F;
    if (painted && instance.barConfig.islands) {
      style.borderWidth = islandStrokeWidth(style.borderWidth);
    }
    box->setStyle(style);
  };
  applyMaterial(instance.bg, !instance.barConfig.islands);
  for (Box* island : instance.islands) {
    if (island != nullptr) {
      applyMaterial(island, true);
    }
  }
}

void Bar::syncBarAutoHideInputRegion(BarInstance& instance) const {
  if (instance.surface == nullptr) {
    return;
  }
  const int surfW = static_cast<int>(instance.surface->width());
  const int surfH = static_cast<int>(instance.surface->height());
  if (!instanceAcceptsPointerInput(instance)) {
    instance.clickThroughInputRegion.reset();
    instance.surface->setInputRegion({});
    return;
  }
  if (barConfigUsesSlideSurface(instance.barConfig)) {
    const bool fullSurface = instance.pointerInside
        || instance.attachedPopupCount > 0
        || instance.hideOpacity > 0.5F
        || (instance.barConfig.smartAutoHide && instance.smartAutoHidePinnedVisible);
    instance.clickThroughInputRegion.reset();
    instance.surface->setInputRegion(barAutoHideSurfaceInputRegion(instance.barConfig, surfW, surfH, fullSurface));
    return;
  }
  const auto barRect = barContentInputRegion(instance.barConfig, m_config->config().shell.shadow, surfW, surfH);
  if (instance.barConfig.deadZone.clickThrough) {
    auto rects = barWidgetsInputRegion(instance, barRect);
    if (instance.clickThroughInputRegion != rects) {
      instance.surface->setInputRegion(rects);
      instance.clickThroughInputRegion = std::move(rects);
    }
    return;
  }
  instance.clickThroughInputRegion.reset();
  instance.surface->setInputRegion({barRect});
}

void Bar::revealAutoHideBar(BarInstance& instance) {
  if (instance.autoHideDisablePending) {
    return;
  }
  if (!barSupportsSlideBehavior(instance.barConfig) || instance.surface == nullptr || instance.slideRoot == nullptr) {
    return;
  }

  instance.autoHideDelayTimer.stop();
  instance.ipcLayoutReleased = false;
  autoLayerMotionStarted(instance);
  const float current = instance.hideOpacity;
  wl_output* output = instance.output;
  const std::string barName = instance.barConfig.name;
  const auto notifyAttachedPanel = [this, inst = &instance, output, barName] {
    finishAutoLayerTransition(*inst);
    PanelManager::instance().onAttachedBarRevealSettled(output, barName);
  };

  constexpr float kSettledThreshold = 0.999F;
  if (current >= kSettledThreshold) {
    const int surfW = static_cast<int>(instance.surface->width());
    const int surfH = static_cast<int>(instance.surface->height());
    instance.clickThroughInputRegion.reset();
    instance.surface->setInputRegion(barAutoHideSurfaceInputRegion(instance.barConfig, surfW, surfH, true));
    syncBarSurfaceChrome(instance);
    instance.surface->requestRedraw();
    notifyAttachedPanel();
    return;
  }

  if (!slideOnCompositor(instance, true, notifyAttachedPanel)) {
    instance.slideAnimId = instance.animations.drive(
        instance.slideAnimId, current, 1.0F, Style::spring(Tokens::springMove),
        [inst = &instance, this](float v) {
          inst->hideOpacity = v;
          syncBarSlideLayerTransform(*inst);
          syncBarSurfaceChrome(*inst);
        },
        notifyAttachedPanel, instance.slideRoot, 0.0F, Threshold::px(static_cast<float>(instance.barConfig.thickness))
    );
  }
  const int surfW = static_cast<int>(instance.surface->width());
  const int surfH = static_cast<int>(instance.surface->height());
  instance.clickThroughInputRegion.reset();
  instance.surface->setInputRegion(barAutoHideSurfaceInputRegion(instance.barConfig, surfW, surfH, true));
  syncBarSurfaceChrome(instance);
  instance.surface->requestRedraw();
}

bool Bar::slideOnCompositor(BarInstance& instance, bool visible, std::function<void()> onSettled) {
  using Property = DskSurfaceClient::Property;
  const bool can = instance.dsk != nullptr && instance.surface != nullptr && MotionService::instance().enabled();
  if (!can) {
    if (instance.compositorSlide) {
      // Hand the slide back to the shell where the compositor left it.
      instance.compositorSlide = false;
      instance.compositorSliding = false;
      if (instance.dsk != nullptr) {
        instance.dsk->set(Property::Translate, {});
      }
    }
    return false;
  }
  instance.animations.cancel(instance.slideAnimId);
  instance.slideAnimId = 0;
  if (!instance.compositorSlide) {
    // Take over from the shell slide: the compositor starts where the shell left the content.
    const double t = 1.0 - instance.hideOpacity;
    instance.dsk->set(Property::Translate, {instance.slideHiddenDx * t, instance.slideHiddenDy * t, 0.0, 0.0});
    instance.compositorSlide = true;
    syncBarSlideLayerTransform(instance);
  }
  instance.compositorSliding = true;
  const double t = visible ? 0.0 : 1.0;
  instance.dsk->animate(
      Property::Translate, visible ? DskSurfaceClient::Curve::SpringStandard : DskSurfaceClient::Curve::SpringClose,
      {instance.slideHiddenDx * t, instance.slideHiddenDy * t, 0.0, 0.0},
      [this, inst = &instance, visible, onSettled = std::move(onSettled)](DskSurfaceClient::EndReason reason) {
        if (reason != DskSurfaceClient::EndReason::Finished) {
          return;
        }
        inst->compositorSliding = false;
        inst->hideOpacity = visible ? 1.0F : 0.0F;
        syncBarSurfaceChrome(*inst);
        if (inst->surface != nullptr) {
          inst->surface->requestRedraw();
        }
        if (onSettled) {
          onSettled();
        }
      }
  );
  syncBarSurfaceChrome(instance);
  instance.surface->requestRedraw();
  return true;
}

void Bar::syncBarSlideLayerTransform(BarInstance& instance) const {
  if (instance.slideRoot == nullptr) {
    return;
  }
  if (instance.compositorSlide) {
    instance.slideRoot->setPosition(0.0F, 0.0F);
    return;
  }
  if (instance.barConfig.autoHide
      || instance.barConfig.smartAutoHide
      || instance.ipcLayoutReleased
      || instance.hideOpacity < 0.999F) {
    const float t = 1.0F - instance.hideOpacity;
    instance.slideRoot->setPosition(instance.slideHiddenDx * t, instance.slideHiddenDy * t);
  } else {
    instance.slideRoot->setPosition(0.0F, 0.0F);
  }
}

void Bar::applyBarCompositorBlur(BarInstance& instance) const {
  if (instance.surface == nullptr) {
    return;
  }
  if (!instance.barConfig.compositorBlur || !barContentVisuallyShown(instance)) {
    instance.surface->clearBlurRegion();
    instance.islandBlurStrips.clear();
    return;
  }

  if (instance.bg == nullptr) {
    return;
  }
  if (instance.barConfig.islands && syncIslandShapes(instance, false)) {
    return;
  }
  if (instance.barConfig.islands) {
    std::vector<InputRect> strips;
    for (const Box* island : instance.islands) {
      // A side island that stepped aside for the overview shrinks to nothing.
      if (island == nullptr || !island->visible() || island->width() < 1.0F || island->height() < 1.0F) {
        continue;
      }
      float x = 0.0F;
      float y = 0.0F;
      Node::absolutePosition(island, x, y);
      // The exact pill: the compositor takes the liquid rim's shape from this region and masks the blur's staircase
      // with its own anti-aliased corners, so an inset would move the rim 1 px inside the edge.
      const auto rects = Surface::tessellateRoundedRect(
          static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)),
          static_cast<int>(std::lround(island->width())), static_cast<int>(std::lround(island->height())),
          std::min(island->width(), island->height()) * 0.5F
      );
      strips.insert(strips.end(), rects.begin(), rects.end());
    }
    if (strips == instance.islandBlurStrips) {
      return;
    }
    instance.surface->setBlurRegion(strips);
    instance.islandBlurStrips = std::move(strips);
    return;
  }
  float absX = 0.0F;
  float absY = 0.0F;
  Node::absolutePosition(instance.bg, absX, absY);
  const int px = static_cast<int>(std::lround(absX));
  const int py = static_cast<int>(std::lround(absY));
  const int pw = static_cast<int>(std::lround(std::max(0.0F, instance.bg->width())));
  const int ph = static_cast<int>(std::lround(std::max(0.0F, instance.bg->height())));
  const auto concave = barConcaveShape(instance.barConfig);
  // The bg node is the visual rect; tessellateShape expects the body rect and
  // expands it outward by logicalInset itself. With all-convex corners and no
  // inset it takes its own fast path down to a plain rounded rect.
  const int insetL = static_cast<int>(std::lround(concave.logicalInset.left));
  const int insetT = static_cast<int>(std::lround(concave.logicalInset.top));
  const int insetR = static_cast<int>(std::lround(concave.logicalInset.right));
  const int insetB = static_cast<int>(std::lround(concave.logicalInset.bottom));
  auto blurStrips = Surface::tessellateShape(
      px + insetL, py + insetT, pw - insetL - insetR, ph - insetT - insetB, concave.corners, concave.logicalInset,
      concave.radii
  );
  if (blurStrips == instance.islandBlurStrips) {
    return;
  }
  instance.surface->setBlurRegion(blurStrips);
  instance.islandBlurStrips = std::move(blurStrips);
}

void Bar::startHideFadeOut(BarInstance& instance, bool immediate) {
  if (instance.autoHideDisablePending || instance.smartAutoHidePinnedVisible) {
    return;
  }
  if (!immediate
      && instance.barConfig.autoHide
      && !instance.barConfig.smartAutoHide
      && instance.barConfig.autoHideDelayMs > 0) {
    if (!instance.autoHideDelayTimer.active()) {
      instance.autoHideDelayTimer.start(
          std::chrono::milliseconds(instance.barConfig.autoHideDelayMs), [this, inst = &instance] {
            if (inst->pointerInside || inst->attachedPopupCount > 0 || !barPointerHideAllowed(*inst)) {
              finishAutoLayerTransition(*inst);
              return;
            }
            const bool suppressed =
                (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*inst) : false;
            if (!suppressed) {
              startHideFadeOut(*inst, true);
            } else {
              finishAutoLayerTransition(*inst);
            }
          }
      );
    }
    return;
  }
  instance.autoHideDelayTimer.stop();
  autoLayerMotionStarted(instance);
  const auto settled = [inst = &instance, this] {
    if (inst->surface == nullptr) {
      return;
    }
    syncBarAutoHideInputRegion(*inst);
    syncBarSurfaceChrome(*inst);
    inst->surface->requestRedraw();
    finishAutoLayerTransition(*inst);
  };
  if (slideOnCompositor(instance, false, settled)) {
    return;
  }
  const float current = instance.hideOpacity;
  instance.slideAnimId = instance.animations.drive(
      instance.slideAnimId, current, 0.0F, Style::spring(Tokens::springClose),
      [this, inst = &instance](float v) {
        inst->hideOpacity = v;
        syncBarSlideLayerTransform(*inst);
        syncBarSurfaceChrome(*inst);
      },
      settled, instance.slideRoot, 0.0F, Threshold::px(static_cast<float>(instance.barConfig.thickness))
  );
  syncBarSurfaceChrome(instance);
  if (instance.surface != nullptr) {
    instance.surface->requestRedraw();
  }
}

BarInstance* Bar::instanceForSurface(Surface* surface) const noexcept {
  if (surface == nullptr) {
    return nullptr;
  }
  const auto it = m_surfaceMap.find(surface);
  return it != m_surfaceMap.end() ? it->second : nullptr;
}

BarInstance* Bar::instanceForOutput(wl_output* output) const noexcept { return instanceForBar(output, {}); }

BarInstance* Bar::instanceForBar(wl_output* output, std::string_view barName) const noexcept {
  if (output == nullptr) {
    return nullptr;
  }

  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->output != output || instance->surface == nullptr) {
      continue;
    }
    if (barName.empty() || instance->barConfig.name == barName) {
      return instance.get();
    }
  }
  return nullptr;
}

