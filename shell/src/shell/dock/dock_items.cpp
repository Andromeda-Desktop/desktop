#include "core/files/resource_paths.h"
#include "shell/dock/dock_items.h"

#include "ui/controls/count_badge.h"
#include "config/app_icon_settings.h"

#include "config/config_service.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/dock/dock_geometry.h"
#include "shell/dock/dock_instance.h"
#include "shell/dock/dock_model.h"
#include "shell/tooltip/tooltip_manager.h"
#include "system/icon_resolver.h"
#include "system/internal_app_metadata.h"
#include "ui/app_icon_colorization.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/layer_surface.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstdint>
#include <linux/input-event-codes.h>
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

namespace {

  // Badge geometry (component.badge at component.dock.icon), scaled with the icon size.
  TOKENS_LIVE(kBadgeSizeRatio, Tokens::componentBadgeHeight / shell::dock::designIconSize());
  constexpr float kBadgeMinSize = 16.0F;
  TOKENS_LIVE(kBadgeFontRatio, Tokens::typeCaption.size / Tokens::componentBadgeHeight);
  TOKENS_LIVE(kBadgeCornerInsetX, 1.0F - Tokens::componentBadgeOverhang / Tokens::componentBadgeHeight);
  TOKENS_LIVE(kBadgeCornerInsetY, Tokens::componentBadgeOverhang / Tokens::componentBadgeHeight);
  constexpr float kDotSizeRatio = 0.09F;
  constexpr float kDotMinSize = 4.0F;
  TOKENS_LIVE(kDotGap, Tokens::spaceP4);
  // Running dot (rulebook 9.4): centred in the bottom padding, so 2 below the icon.
  TOKENS_LIVE(kRunningDot, Tokens::componentDockRunningDot);
  TOKENS_LIVE(kRunningDotOffset, (Tokens::componentDockPadding - Tokens::componentDockRunningDot) * 0.5F);
  // component.dock.padding and .gap space the icons; a cell adds none.
  constexpr float kCellPad = 0.0F;
  constexpr float kHoverZoomLerp = 0.28F;
  constexpr float kHoverZoomEaseMs = 180.0F;
  constexpr float kHoverZoomReferenceFrameMs = 1000.0F / 60.0F;
  // Pointer hit padding (in item pitches) — keep generous so edge icons still magnify.
  constexpr float kHoverZoomInfluence = 2.25F;
  // Scale falloff radius — tighter than hit padding so neighbors stay closer to rest size.
  constexpr float kHoverZoomFalloffInfluence = 1.5F;
  constexpr std::int32_t kHoverZoomZScale = 100;
  // Pinned caps of a scrolling dock draw and take input above every app, zoomed or not.
  constexpr std::int32_t kPinnedCapZ = 1 << 20;
  constexpr auto kDragHoldDelay = std::chrono::milliseconds(300);

  [[nodiscard]] float pointerMainOnRow(const DockConfig& cfg, const InputArea* area, float localX, float localY) {
    if (area == nullptr) {
      return 0.0F;
    }
    const bool vertical = shell::dock::isVerticalEdge(cfg.position);
    const float areaMain = vertical ? area->y() + area->translationY() : area->x() + area->translationX();
    return areaMain + (vertical ? localY : localX);
  }

  void resetDockItemDragState(shell::dock::DockInstance& instance) {
    instance.drag.holdTimer.stop();
    instance.drag.apply(shell::dock::DragEvent::Reset);
    instance.drag.clearPayload();
  }

  void dismissDockTooltipImpl() {
    TooltipManager::instance().onHoverChange(nullptr, nullptr, OutputId::None);
  }

  [[nodiscard]] int dockIconDecodeTargetSize(const DockConfig& cfg) {
    const float baseScale = std::max(cfg.activeScale, cfg.inactiveScale);
    const float hoverScale = cfg.magnification ? std::max(1.0F, cfg.magnificationScale) : 1.0F;
    const float peakScale = std::max(1.0F, baseScale * hoverScale);
    return std::max(1, static_cast<int>(std::round(static_cast<float>(cfg.iconSize) * peakScale)));
  }

  [[nodiscard]] float glyphCell(const DockConfig& cfg) {
    return static_cast<float>(shell::dock::dockGlyphCellSize(cfg));
  }

  // A launcher or trash glyph node is glyphCell square: flush on the main axis, centred on the cross axis.
  [[nodiscard]] float glyphBaseX(const DockConfig& cfg) {
    return kCellPad + (shell::dock::isVerticalEdge(cfg.position)
                           ? std::round((static_cast<float>(cfg.iconSize) - glyphCell(cfg)) * 0.5F)
                           : 0.0F);
  }

  [[nodiscard]] float glyphBaseY(const DockConfig& cfg) {
    return kCellPad + (shell::dock::isVerticalEdge(cfg.position)
                           ? 0.0F
                           : std::round((static_cast<float>(cfg.iconSize) - glyphCell(cfg)) * 0.5F));
  }

  [[nodiscard]] float launcherIconSize(const DockConfig& cfg) {
    return cfg.launcherCustomImage.empty() ? glyphCell(cfg) : static_cast<float>(cfg.iconSize);
  }

  [[nodiscard]] float launcherIconBaseX(const DockConfig& cfg) {
    return cfg.launcherCustomImage.empty() ? glyphBaseX(cfg) : kCellPad;
  }

  [[nodiscard]] float launcherIconBaseY(const DockConfig& cfg) {
    return cfg.launcherCustomImage.empty() ? glyphBaseY(cfg) : kCellPad;
  }

  void applyHoverIconVisual(Node* iconNode, DockEdge edge, float baseX, float baseY, float iconSize, float scale) {
    if (iconNode == nullptr) {
      return;
    }
    iconNode->setScale(scale);
    const float shift = scale > 1.0F ? iconSize * (1.0F - scale) * 0.5F : 0.0F;
    float x = baseX;
    float y = baseY;
    shell::dock::shiftAlongEdge(edge, x, y, shift);
    iconNode->setPosition(x, y);
  }

  void applyBadgeVisual(
      Box* badge, DockEdge edge, float iconBaseX, float iconBaseY, float iconSize, float badgeSize, float iconScale
  ) {
    if (badge == nullptr) {
      return;
    }
    badge->setScale(iconScale);
    const float shift = iconScale > 1.0F ? iconSize * (1.0F - iconScale) * 0.5F : 0.0F;
    float iconX = iconBaseX;
    float iconY = iconBaseY;
    shell::dock::shiftAlongEdge(edge, iconX, iconY, shift);
    const float iconLeft = iconX + iconSize * (1.0F - iconScale) * 0.5F;
    const float iconRight = iconX + iconSize * (1.0F + iconScale) * 0.5F;
    const float iconTop = iconY + iconSize * (1.0F - iconScale) * 0.5F;
    const float iconBottom = iconY + iconSize * (1.0F + iconScale) * 0.5F;
    const float scaledBadgeSize = badgeSize * iconScale;
    // Keep the badge on the screen-interior corner so a flush dock never needs an outer-edge gap.
    const float badgeLeft = edge == DockEdge::Right ? iconLeft - scaledBadgeSize * (1.0F - kBadgeCornerInsetX)
                                                    : iconRight - scaledBadgeSize * kBadgeCornerInsetX;
    const float badgeTop = edge == DockEdge::Top ? iconBottom - scaledBadgeSize * (1.0F - kBadgeCornerInsetY)
                                                 : iconTop - scaledBadgeSize * kBadgeCornerInsetY;
    const float badgeCenterAdjust = badgeSize * (1.0F - iconScale) * 0.5F;
    badge->setPosition(badgeLeft - badgeCenterAdjust, badgeTop - badgeCenterAdjust);
  }

  void applyHoverItemVisual(
      Node* iconNode, Box* badge, DockEdge edge, float iconBaseX, float iconBaseY, float iconSize, float badgeSize,
      float scale
  ) {
    applyHoverIconVisual(iconNode, edge, iconBaseX, iconBaseY, iconSize, scale);
    applyBadgeVisual(badge, edge, iconBaseX, iconBaseY, iconSize, badgeSize, scale);
  }

  void applyShellAppIconColorization(Image* image, const ShellConfig& shell) {
    if (image == nullptr) {
      return;
    }
    image->setAppIconColorization(effectiveShellAppIconColorizationTint(shell));
  }

  [[nodiscard]] float itemRestCenterMain(float restMainPos, float cellMain) { return restMainPos + cellMain * 0.5F; }

  // The row layout owns an item's position; hover, drag and glide move it by a translation layout never writes.
  void applyItemMainOffset(Node* node, bool vertical, float offset) {
    if (node == nullptr) {
      return;
    }
    node->setTranslation(vertical ? 0.0F : offset, vertical ? offset : 0.0F);
  }

  [[nodiscard]] float hoverZoomFrameLerp(float deltaMs) {
    const float clampedMs = std::clamp(deltaMs, 1.0F, 50.0F);
    return 1.0F - std::pow(1.0F - kHoverZoomLerp, clampedMs / kHoverZoomReferenceFrameMs);
  }

  [[nodiscard]] bool lerpHoverMainOffset(float& current, float target, float lerpFactor) {
    if (std::abs(current - target) <= 0.25F) {
      if (current != target) {
        current = target;
        return true;
      }
      return false;
    }
    current += (target - current) * lerpFactor;
    return true;
  }

  void clampSpreadOffsetsToBounds(
      const std::vector<float>& restMainPos, const std::vector<float>& scales, const std::vector<float>& sizes,
      float boundsMin, float boundsMax, std::vector<float>& offsets
  ) {
    if (offsets.empty() || restMainPos.size() != offsets.size() || scales.size() != offsets.size()) {
      return;
    }

    const auto restVisualMainMax = [&](std::size_t index) {
      return restMainPos[index] + sizes[index] * 0.5F + sizes[index] * scales[index] * 0.5F;
    };
    const auto restVisualMainMin = [&](std::size_t index) {
      return restMainPos[index] + sizes[index] * 0.5F - sizes[index] * scales[index] * 0.5F;
    };

    float spreadFactor = 1.0F;
    for (std::size_t index = 0; index < offsets.size(); ++index) {
      const float offset = offsets[index];
      if (std::abs(offset) <= 0.001F) {
        continue;
      }
      if (offset > 0.0F) {
        const float maxRightOffset = boundsMax - restVisualMainMax(index);
        if (maxRightOffset <= 0.0F) {
          spreadFactor = 0.0F;
        } else {
          spreadFactor = std::min(spreadFactor, maxRightOffset / offset);
        }
      } else {
        const float maxLeftOffset = boundsMin - restVisualMainMin(index);
        if (maxLeftOffset >= 0.0F) {
          spreadFactor = 0.0F;
        } else {
          spreadFactor = std::min(spreadFactor, maxLeftOffset / offset);
        }
      }
    }

    spreadFactor = std::clamp(spreadFactor, 0.0F, 1.0F);
    if (spreadFactor >= 1.0F) {
      return;
    }
    for (float& offset : offsets) {
      offset *= spreadFactor;
    }
  }

  void applyHoverZoomZIndex(InputArea* area, float scale) {
    if (area == nullptr) {
      return;
    }
    area->setZIndex(static_cast<std::int32_t>(std::lround(scale * static_cast<float>(kHoverZoomZScale))));
  }

  [[nodiscard]] TooltipAnchorInsets
  scaledIconTooltipAnchor(
      const InputArea* area, DockEdge edge, float iconBaseX, float iconBaseY, float iconSize, float scale
  ) {
    const float centerAdjust = iconSize * (1.0F - scale) * 0.5F;
    float iconX = iconBaseX;
    float iconY = iconBaseY;
    const float shift = iconSize * (1.0F - scale) * 0.5F;
    shell::dock::shiftAlongEdge(edge, iconX, iconY, shift);
    const float left = iconX + centerAdjust;
    const float top = iconY + centerAdjust;
    const float visualSize = iconSize * scale;
    return TooltipAnchorInsets{
        .top = top,
        .right = std::max(0.0F, area->width() - left - visualSize),
        .bottom = std::max(0.0F, area->height() - top - visualSize),
        .left = left,
    };
  }

  void syncHoveredTooltipAnchor(
      InputArea* area, const DockConfig& cfg, float iconBaseX, float iconBaseY, float iconSize, float scale
  ) {
    if (area == nullptr || !area->hovered()) {
      return;
    }
    area->setTooltipAnchorInsets(
        scaledIconTooltipAnchor(area, cfg.position, iconBaseX, iconBaseY, iconSize, scale)
    );
    TooltipManager::instance().syncAnchor(area);
  }

  [[nodiscard]] float computeHoverMultiplier(
      float pointerMain, float itemCenterMain, float itemPitch, float maxMultiplier, float falloffInfluence
  ) {
    if (maxMultiplier <= 1.0F) {
      return 1.0F;
    }
    const float distance = std::abs(pointerMain - itemCenterMain);
    const float influence = itemPitch * falloffInfluence;
    if (distance >= influence) {
      return 1.0F;
    }
    const float t = distance / influence;
    const float cosine = std::cos(t * std::numbers::pi_v<float> * 0.5F);
    // Squared cosine keeps the center pop but drops neighbors toward rest scale faster.
    const float falloff = cosine * cosine;
    return 1.0F + (maxMultiplier - 1.0F) * falloff;
  }

  [[nodiscard]] std::string_view dockEdgeKey(DockEdge edge) {
    switch (edge) {
    case DockEdge::Top:
      return "top";
    case DockEdge::Bottom:
      return "bottom";
    case DockEdge::Left:
      return "left";
    case DockEdge::Right:
      return "right";
    }
    return "bottom";
  }

  void setDockTooltipText(InputArea& area, std::string text) {
    if (text.empty()) {
      area.clearTooltip();
    } else {
      area.setTooltip(std::move(text));
    }
  }

  void configureDockTooltip(InputArea& area, const DockConfig& cfg, std::string text) {
    setDockTooltipText(area, std::move(text));
    area.setTooltipPlacement(tooltipPlacementAwayFromEdge(dockEdgeKey(cfg.position)));
    area.setTooltipAnchorInsets(
        TooltipAnchorInsets{
            .top = kCellPad,
            .right = kCellPad,
            .bottom = kCellPad,
            .left = kCellPad,
        }
    );
  }

  [[nodiscard]] std::string dockItemTooltipText(const shell::dock::DockItemModel& model) {
    // A running app shows its window previews on hover; their header names it, and a label would cover them.
    if (model.running) {
      return {};
    }
    const auto& entry = model.entry;
    std::string text = !entry.name.empty() ? entry.name : !entry.genericName.empty() ? entry.genericName : entry.id;
    if (model.minimizedCount != 0) {
      text += '\n';
      text += model.instanceCount == 1 ? i18n::tr("common.minimized")
                                       : i18n::tr("common.minimized-count", "count", model.minimizedCount);
    }
    return text;
  }

  void applyAnimatedIconScale(
      shell::dock::DockInstance& instance, Node* iconNode, float& visualScale, AnimationManager::Id& animId,
      float targetScale
  ) {
    if (iconNode == nullptr) {
      return;
    }
    if (visualScale < 0.0F) {
      visualScale = targetScale;
      iconNode->setScale(targetScale);
      return;
    }
    if (std::abs(visualScale - targetScale) <= 0.001F) {
      return;
    }
    if (animId != 0) {
      instance.animations.cancel(animId);
    }
    const Tokens::Spring& token = targetScale < visualScale ? Tokens::springPress : Tokens::springRelease;
    animId = instance.animations.animateSpatial(
        visualScale, targetScale, Style::spring(token),
        [node = iconNode, visualScalePtr = &visualScale](float value) {
          *visualScalePtr = value;
          node->setScale(value);
        },
        [animIdPtr = &animId] { *animIdPtr = 0; }
    );
  }

  [[nodiscard]] bool applyHoverIconScale(
      Node* iconNode, float& visualScale, AnimationManager::Id& animId, shell::dock::DockInstance& instance,
      float targetScale, float restScale, float lerpFactor, DockEdge edge, float baseX, float baseY, float iconSize,
      Box* badge, float badgeSize, bool relaid
  ) {
    if (iconNode == nullptr) {
      return false;
    }
    if (animId != 0) {
      instance.animations.cancel(animId);
      animId = 0;
    }
    if (visualScale < 0.0F) {
      visualScale = restScale;
      applyHoverItemVisual(iconNode, badge, edge, baseX, baseY, iconSize, badgeSize, restScale);
    } else if (relaid) {
      applyHoverItemVisual(iconNode, badge, edge, baseX, baseY, iconSize, badgeSize, visualScale);
    }
    if (std::abs(visualScale - targetScale) <= 0.002F) {
      if (std::abs(visualScale - targetScale) > 0.0F) {
        visualScale = targetScale;
        applyHoverItemVisual(iconNode, badge, edge, baseX, baseY, iconSize, badgeSize, targetScale);
      }
      return false;
    }
    const float next = visualScale + (targetScale - visualScale) * lerpFactor;
    visualScale = next;
    applyHoverItemVisual(iconNode, badge, edge, baseX, baseY, iconSize, badgeSize, next);
    return true;
  }

} // namespace

namespace shell::dock {

  struct DockItemClickContext {
    DockItemCallbacks callbacks;
  };

  std::string_view dockLauncherIconGlyph(const DockConfig& cfg) {
    return cfg.launcherIcon.empty() ? "grain" : std::string_view{cfg.launcherIcon};
  }

  std::unique_ptr<Flex> makeDockItemRow(const DockConfig& cfg, bool vertical) {
    const auto mainPad = static_cast<float>(cfg.mainAxisPadding);
    const auto crossPad = static_cast<float>(cfg.crossAxisPadding);
    auto row = ui::flex(
        vertical ? FlexDirection::Vertical : FlexDirection::Horizontal,
        {
            .align = FlexAlign::Center,
            .gap = static_cast<float>(cfg.itemSpacing),
            .paddingV = vertical ? mainPad : crossPad,
            .paddingH = vertical ? crossPad : mainPad,
        }
    );
    row->setMirrorInRtl(false);
    return row;
  }

  void handleItemClick(DockInstance& instance, const DockItemAction& action, DockItemClickContext& context);

  // The focused app sits on the lens (rulebook 6, 9.4): the icon rect grown by the lens inset.
  void syncDockLens(shell::dock::DockInstance& instance, const shell::dock::DockSnapshot& snapshot) {
    const auto& cfg = instance.config;
    const auto iSize = static_cast<float>(cfg.iconSize);
    const float k = iSize / shell::dock::designIconSize();
    const float inset = Tokens::componentDockLensInset * k;
    const bool vert = shell::dock::isVerticalEdge(cfg.position);
    const std::size_t count = std::min(instance.items.size(), snapshot.items.size());
    for (std::size_t i = 0; i < count; ++i) {
      const auto& item = instance.items[i];
      if (snapshot.items[i].active && item.area != nullptr) {
        const float main = item.restMainPos;
        const float cross = item.restCrossPos;
        const bool animate = instance.lensActiveIndex != i;
        instance.lensActiveIndex = i;
        instance.lens.moveTo(
            main - inset, main + iSize + inset, cross - inset, iSize + 2.0F * inset,
            Style::scaledRadius(Tokens::componentDockLensRadius, k), animate
        );
        return;
      }
    }
    instance.lensActiveIndex = static_cast<std::size_t>(-1);
    instance.lens.hide();
  }

  std::unique_ptr<InputArea> createLauncherButton(
      DockInstance& instance, const DockConfig& cfg, Renderer& renderer,
      const std::shared_ptr<DockItemClickContext>& clickContext
  ) {
    const DockEdge edge = cfg.position;
    const bool vert = shell::dock::isVerticalEdge(edge);
    const auto iSize = static_cast<float>(cfg.iconSize);
    const float cellMain = launcherIconSize(cfg) + 2.0F * kCellPad;
    const float cellCross = iSize + 2.0F * kCellPad;
    const int iconDecodeTarget = dockIconDecodeTargetSize(cfg);

    auto areaNode = ui::inputArea({});
    if (!vert) {
      areaNode->setSize(cellMain, cellCross);
    } else {
      areaNode->setSize(cellCross, cellMain);
    }

    if (!cfg.launcherCustomImage.empty()) {
      auto launcherImage = ui::image({
          .fit = ImageFit::Contain,
          .width = iSize,
          .height = iSize,
          .configure = [&cfg, iSize, &renderer, iconDecodeTarget](Image& image) {
            image.load(renderer, cfg.launcherCustomImage, {.targetSize = iconDecodeTarget, .mipmap = true});
            image.setForegroundTint(
                cfg.launcherCustomImageColorize ? std::optional<ColorSpec>{colorSpecFromRole(ColorRole::Label1)}
                                                : std::nullopt
            );
            image.setPosition(kCellPad, kCellPad);
          },
      });
      instance.launcherIconNode = static_cast<Image*>(launcherImage.get());
      areaNode->addChild(std::move(launcherImage));
    } else {
      const float g = glyphCell(cfg);
      auto launcherGlyph = ui::glyph({
          .glyphSize = g,
          .color = colorSpecFromRole(ColorRole::Label1),
          .width = g,
          .height = g,
          .configure = [&cfg](Glyph& glyph) {
            if (!glyph.setGlyph(dockLauncherIconGlyph(cfg))) {
              glyph.setGlyph("grain");
            }
            glyph.setPosition(glyphBaseX(cfg), glyphBaseY(cfg));
          },
      });
      instance.launcherIconNode = static_cast<Glyph*>(launcherGlyph.get());
      areaNode->addChild(std::move(launcherGlyph));
    }
    instance.launcherArea = areaNode.get();
    instance.launcherVisualScale = cfg.inactiveScale;
    instance.launcherIconNode->setScale(cfg.inactiveScale);
    if (instance.launcherScaleAnimId != 0) {
      instance.animations.cancel(instance.launcherScaleAnimId);
      instance.launcherScaleAnimId = 0;
    }

    auto* instPtr = &instance;
    configureDockTooltip(*areaNode, cfg, i18n::tr("dock.launcher"));
    areaNode->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT}));
    areaNode->setOnClick([instPtr, clickContext](const InputArea::PointerData& d) {
      if (d.button == BTN_LEFT && clickContext->callbacks.toggleLauncher) {
        clickContext->callbacks.toggleLauncher(*instPtr);
      }
    });

    return areaNode;
  }

  std::unique_ptr<Box> makeDockSeparator(bool vert, float scale) {
    const float length = std::round(Tokens::componentDockSeparatorHeight * scale);
    return ui::box({
        .fill = colorSpecFromRole(ColorRole::Separator),
        .width = vert ? length : 1.0F,
        .height = vert ? 1.0F : length,
    });
  }

  std::unique_ptr<InputArea> createTrashButton(
      DockInstance& instance, const DockConfig& cfg, DockItemSceneDependencies deps,
      const std::shared_ptr<DockItemClickContext>& clickContext
  ) {
    const float g = glyphCell(cfg);
    const float cellMain = g + 2.0F * kCellPad;
    const float cellCross = static_cast<float>(cfg.iconSize) + 2.0F * kCellPad;

    auto areaNode = ui::inputArea({});
    if (shell::dock::isVerticalEdge(cfg.position)) {
      areaNode->setSize(cellCross, cellMain);
    } else {
      areaNode->setSize(cellMain, cellCross);
    }

    // Andromeda's own glass bin, decoded at its magnified size like app icons.
    const int decode = std::max(
        1, static_cast<int>(std::round(static_cast<float>(dockIconDecodeTargetSize(cfg)) * g / static_cast<float>(cfg.iconSize)))
    );
    instance.trash.themeIcons = {paths::assetPath("dock/trash.svg").string(), paths::assetPath("dock/trash-full.svg").string()};
    instance.trash.decodeSize = decode;
    std::unique_ptr<Node> icon;
    if (!instance.trash.themeIcons[0].empty()) {
      icon = ui::image({
          .width = g,
          .height = g,
          .configure = [&cfg, &shell = deps.model.config.config().shell](Image& image) {
            image.setAppIconColorization(effectiveShellAppIconColorizationTint(shell));
            image.setPosition(glyphBaseX(cfg), glyphBaseY(cfg));
          },
      });
    } else {
      icon = ui::glyph({
          .glyphSize = g,
          .color = colorSpecFromRole(ColorRole::Label1),
          .width = g,
          .height = g,
          .configure = [&cfg](Glyph& node) { node.setPosition(glyphBaseX(cfg), glyphBaseY(cfg)); },
      });
    }
    instance.trash.icon = icon.get();
    areaNode->addChild(std::move(icon));
    instance.trash.area = areaNode.get();
    applyTrashIcon(instance);
    instance.trash.visualScale = cfg.inactiveScale;
    instance.trash.icon->setScale(cfg.inactiveScale);

    auto* instPtr = &instance;
    configureDockTooltip(*areaNode, cfg, i18n::tr("dock.trash"));
    areaNode->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
    areaNode->setOnClick([instPtr, clickContext](const InputArea::PointerData& d) {
      if (d.button == BTN_LEFT && clickContext->callbacks.openTrash) {
        clickContext->callbacks.openTrash(*instPtr);
      } else if (d.button == BTN_RIGHT && clickContext->callbacks.openTrashMenu) {
        dismissDockTooltipImpl();
        clickContext->callbacks.openTrashMenu(*instPtr);
      }
    });
    return areaNode;
  }

  void applyTrashIcon(DockInstance& instance) {
    if (instance.trash.icon == nullptr) {
      return;
    }
    if (instance.trash.themeIcons[0].empty()) {
      static_cast<Glyph*>(instance.trash.icon)->setGlyph(instance.trashFull ? "trash-x" : "trash");
    } else if (instance.surface != nullptr) {
      static_cast<Image*>(instance.trash.icon)
          ->load(
              instance.surface->renderTarget().renderer(), instance.trash.themeIcons[instance.trashFull ? 1 : 0],
              {.targetSize = instance.trash.decodeSize, .mipmap = true}
          );
    }
    if (instance.surface != nullptr) {
      instance.surface->requestRedraw();
    }
  }

  void rebuildItems(
      DockInstance& instance, DockItemSceneDependencies deps, const DockSnapshot& snapshot,
      const DockItemCallbacks& callbacks
  ) {
    uiAssertNotRendering("shell::dock::rebuildItems");
    if (instance.row == nullptr) {
      return;
    }

    if (instance.surface == nullptr) {
      return;
    }
    Renderer& renderer = instance.surface->renderTarget().renderer();

    resetDockItemDragState(instance);

    const bool vertical = shell::dock::isVerticalEdge(instance.config.position);
    instance.config.iconSize = shell::dock::dockFitIconSize(
        instance.config, instance.restIconSize,
        snapshot.items.size() + shell::dock::dockLauncherButtonCount(instance.config) + instance.trashCells(),
        instance.separatorCount(), shell::dock::dockGlyphCells(instance.config, instance.trashCells()),
        vertical ? instance.outputLogicalHeight : instance.outputLogicalWidth
    );
    const auto& cfg = instance.config;
    const DockEdge edge = cfg.position;
    const DockLauncherPosition launcherPosition = cfg.launcherPosition;
    const bool vert = shell::dock::isVerticalEdge(edge);
    const auto iSize = static_cast<float>(cfg.iconSize);
    const int iconDecodeTarget = dockIconDecodeTargetSize(cfg);
    auto clickContext = std::make_shared<DockItemClickContext>(DockItemClickContext{
        .callbacks = callbacks,
    });

    instance.glideFrom.clear();
    for (auto& item : instance.items) {
      if (item.area != nullptr && !item.idLower.empty() && !item.isDragGhost) {
        instance.glideFrom[item.idLower] = item.restMainPos + item.glideMainOffset;
      }
      if (item.glideAnimId != 0) {
        instance.animations.cancel(item.glideAnimId);
        item.glideAnimId = 0;
      }
      if (item.enterAnimId != 0) {
        instance.animations.cancel(item.enterAnimId);
        item.enterAnimId = 0;
      }
      if (item.scaleAnimId != 0) {
        instance.animations.cancel(item.scaleAnimId);
        item.scaleAnimId = 0;
      }
      if (item.opacityAnimId != 0) {
        instance.animations.cancel(item.opacityAnimId);
        item.opacityAnimId = 0;
      }
    }

    // Clear previous items by recreating the row.
    if (instance.row != nullptr && instance.row->parent() != nullptr) {
      instance.row->parent()->removeChild(instance.row);
      instance.row = nullptr;
    }
    instance.items.clear();
    instance.separators.clear();
    instance.launcherArea = nullptr;
    instance.launcherIconNode = nullptr;
    instance.launcherVisualScale = -1.0F;
    if (instance.launcherScaleAnimId != 0) {
      instance.animations.cancel(instance.launcherScaleAnimId);
      instance.launcherScaleAnimId = 0;
    }
    if (instance.trash.scaleAnimId != 0) {
      instance.animations.cancel(instance.trash.scaleAnimId);
    }
    instance.trash = {};

    auto freshRow = makeDockItemRow(cfg, vert);
    Node* rowParent = instance.viewport != nullptr ? static_cast<Node*>(instance.viewport)
        : instance.slideRoot != nullptr            ? instance.slideRoot
                                                   : static_cast<Node*>(instance.panel);
    instance.row = static_cast<Flex*>(rowParent->addChild(std::move(freshRow)));
        instance.lens.attach(*instance.row, &instance.animations, vert, iSize / shell::dock::designIconSize());
    const auto& itemModels = snapshot.items;

    if (launcherPosition == DockLauncherPosition::Start) {
      instance.row->addChild(createLauncherButton(instance, cfg, renderer, clickContext));
    }

    // Reserve up-front so emplace_back never reallocates while lambdas hold raw pointers.
    instance.items.reserve(itemModels.size());
    const std::size_t pinnedCount = snapshot.pinnedCount;

    for (std::size_t itemIndex = 0; itemIndex < itemModels.size(); ++itemIndex) {
      if (itemIndex == pinnedCount && pinnedCount > 0) {
        instance.separators.push_back({.node = instance.row->addChild(makeDockSeparator(vert, cfg.scale))});
      }
      const auto& model = itemModels[itemIndex];
      auto& item = instance.items.emplace_back();
      item.idLower = model.idLower;
      DockItemAction action{
          .entry = model.entry,
          .idLower = model.idLower,
          .startupWmClassLower = model.startupWmClassLower,
          .windowLookupIdLower = model.windowLookupIdLower,
          .windowLookupWmClassLower = model.windowLookupWmClassLower,
      };

      const float cellMain = iSize + 2.0F * kCellPad;
      const float cellCross = iSize + 2.0F * kCellPad;
      auto areaNode = ui::inputArea({});
      if (!vert) {
        areaNode->setSize(cellMain, cellCross);
      } else {
        areaNode->setSize(cellCross, cellMain);
      }

      std::string iconPath;
      if (!model.entry.icon.empty()) {
        iconPath = deps.iconResolver.resolve(model.entry.icon, iconDecodeTarget);
      }
      if (iconPath.empty()) {
        if (const auto internal = internal_apps::metadataForDesktopEntry(model.entry); internal.has_value()) {
          iconPath = internal->iconPath;
        }
      }
      if (iconPath.empty()) {
        iconPath = deps.iconResolver.resolve("application-x-executable", iconDecodeTarget);
      }
      auto iconImg = ui::image({
          .width = iSize,
          .height = iSize,
          .configure = [&renderer, iconPath, iconDecodeTarget,
                        &shell = deps.model.config.config().shell](Image& image) {
            image.setAppIconColorization(effectiveShellAppIconColorizationTint(shell));
            if (!iconPath.empty()) {
              image.load(renderer, iconPath, {.targetSize = iconDecodeTarget, .mipmap = true});
            }
            image.setPosition(kCellPad, kCellPad);
          },
      });

      if (iconImg->loadState() != ImageLoad::Failed) {
        item.iconImage = static_cast<Image*>(areaNode->addChild(std::move(iconImg)));
      } else {
        item.iconGlyph = static_cast<Glyph*>(areaNode->addChild(
            ui::glyph({
                .glyph = "app-window",
                .glyphSize = iSize,
                .color = colorSpecFromRole(ColorRole::Label1),
                .width = iSize,
                .height = iSize,
                .configure = [](Glyph& glyph) { glyph.setPosition(kCellPad, kCellPad); },
            })
        ));
      }

      if (cfg.showDots) {
        const float dot = std::max(kDotMinSize * cfg.scale, std::round(iSize * kDotSizeRatio));
        const bool verticalDots = shell::dock::isVerticalEdge(edge);

        for (auto& dotIndicator : item.dotIndicators) {
          dotIndicator = static_cast<Box*>(areaNode->addChild(
              ui::box({
                  .fill = colorSpecFromRole(ColorRole::Accent),
                  .radius = dot * 0.5F,
                  .width = dot,
                  .height = dot,
                  .visible = false,
                  .configure = [verticalDots, edge, cellMain, dot](Box& box) {
                    if (verticalDots) {
                      const float x = edge == DockEdge::Left ? 1.0F : std::round(cellMain - dot - 1.0F);
                      box.setPosition(x, std::round((cellMain - dot) * 0.5F));
                    } else {
                      const float y = edge == DockEdge::Bottom ? std::round(cellMain - dot - 1.0F) : 1.0F;
                      box.setPosition(std::round((cellMain - dot) * 0.5F), y);
                    }
                  },
              })
          ));
        }
      }

      if (cfg.runningBar && !cfg.showDots) {
        const float dot = std::round(kRunningDot * cfg.scale);
        const float offset = std::round(kRunningDotOffset * cfg.scale);
        areaNode->addChild(
            ui::box({
                .out = &item.runningIndicator,
                .fill = colorSpecFromRole(ColorRole::Label2),
                .radius = dot * 0.5F,
                .width = dot,
                .height = dot,
                .visible = false,
                .participatesInLayout = false,
                .configure = [vert, edge, cellMain, dot, offset](Box& box) {
                  const float centered = std::round((cellMain - dot) * 0.5F);
                  const bool atStart = edge == DockEdge::Top || edge == DockEdge::Left;
                  const float outer = atStart ? -(offset + dot) : cellMain + offset;
                  box.setPosition(vert ? outer : centered, vert ? centered : outer);
                },
            })
        );
      }

      {
        const float bd = std::max(kBadgeMinSize, iSize * kBadgeSizeRatio);

        areaNode->addChild(
            ui::box({
                .out = &item.badge,
                .radius = bd * 0.5F,
                .width = bd,
                .height = bd,
                .visible = false,
                .configure = [edge, iSize, bd](Box& box) {
                  applyBadgeVisual(&box, edge, kCellPad, kCellPad, iSize, bd, 1.0F);
                },
            })
        );

        item.badge->addChild(
            ui::label({
                .out = &item.badgeLabel,
                .fontSize = bd * kBadgeFontRatio,
                .fontWeight = static_cast<FontWeight>(Tokens::typeHeadline.weight),
                .maxLines = 1,
                .visible = false,
            })
        );
      }

      auto* instPtr = &instance;

      item.tooltipText = dockItemTooltipText(model);
      configureDockTooltip(*areaNode, cfg, item.tooltipText);
      areaNode->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
      const bool itemPinned = itemIndex < pinnedCount;
      areaNode->setOnPress([instPtr, clickContext, itemIndex, itemPinned](const InputArea::PointerData& d) {
        if (d.button != BTN_LEFT) {
          return;
        }
        if (itemIndex < instPtr->items.size()) {
          // Press scales the cell to 0.97 and release springs back.
          auto& pressed = instPtr->items[itemIndex];
          if (pressed.area != nullptr) {
            const Tokens::Spring& token = d.pressed ? Tokens::springPress : Tokens::springRelease;
            if (pressed.pressAnimId != 0) {
              instPtr->animations.cancel(pressed.pressAnimId);
            }
            pressed.pressAnimId = instPtr->animations.animateSpatial(
                pressed.pressScale, d.pressed ? 0.97F : 1.0F,
                Style::spring(token),
                [item = &pressed](float v) {
                  item->pressScale = v;
                  item->area->setTransformOrigin(item->area->width() * 0.5F, item->area->height() * 0.5F);
                  item->area->setScale(v);
                },
                [item = &pressed] { item->pressAnimId = 0; }, pressed.area
            );
          }
        }
        auto& drag = instPtr->drag;
        const auto& dockCfg = instPtr->config;
        if (d.pressed) {
          if (!itemPinned) {
            return;
          }
          resetDockItemDragState(*instPtr);
          drag.apply(DragEvent::Press);
          drag.pinned = true;
          drag.sourceIndex = itemIndex;
          drag.startMain = pointerMainOnRow(dockCfg, instPtr->items[itemIndex].area, d.localX, d.localY);
          drag.currentMain = drag.startMain;
          drag.targetIndex = itemIndex;
          drag.holdTimer.start(kDragHoldDelay, [instPtr, itemIndex] {
            if (instPtr->drag.sourceIndex == itemIndex && instPtr->drag.apply(DragEvent::HoldElapsed)) {
              dismissDockTooltipImpl();
            }
          });
          return;
        }

        if (drag.sourceIndex != itemIndex) {
          return;
        }

        if (drag.active()) {
          if (clickContext->callbacks.endDrag) {
            clickContext->callbacks.endDrag(*instPtr, true);
          }
          return;
        }

        drag.holdTimer.stop();
        if (drag.phase == DragPhase::Armed) {
          if (clickContext->callbacks.endDrag) {
            clickContext->callbacks.endDrag(*instPtr, false);
          }
        } else {
          drag.apply(DragEvent::Release);
          resetDockItemDragState(*instPtr);
        }
      });
      areaNode->setOnMotion([instPtr, clickContext, itemIndex, itemPinned](const InputArea::PointerData& d) {
        if (!itemPinned || instPtr->drag.sourceIndex != itemIndex) {
          return;
        }
        if (!instPtr->drag.engaged()) {
          return;
        }

        const auto& dockCfg = instPtr->config;
        const float mainPos = pointerMainOnRow(dockCfg, instPtr->items[itemIndex].area, d.localX, d.localY);
        instPtr->drag.currentMain = mainPos;
        if (instPtr->drag.active()) {
          if (clickContext->callbacks.updateDrag) {
            clickContext->callbacks.updateDrag(*instPtr, mainPos);
          }
        } else if (instPtr->drag.phase == DragPhase::Armed && clickContext->callbacks.beginDrag) {
          clickContext->callbacks.beginDrag(*instPtr, itemIndex, mainPos);
        }
      });
      areaNode->setOnClick([action = std::move(action), instPtr, clickContext,
                            item = static_cast<Node*>(areaNode.get())](const InputArea::PointerData& d) {
        if (d.button == BTN_RIGHT) {
          if (clickContext->callbacks.openItemMenu) {
            dismissDockTooltipImpl();
            clickContext->callbacks.openItemMenu(*instPtr, action);
          }
          return;
        }
        if (d.button != BTN_LEFT) {
          return;
        }
        if (instPtr->suppressItemClick) {
          instPtr->suppressItemClick = false;
          return;
        }
        if (instPtr->drag.engaged()) {
          return;
        }
        instPtr->clickedItem = item;
        handleItemClick(*instPtr, action, *clickContext);
        instPtr->clickedItem = nullptr;
      });

      item.area = static_cast<InputArea*>(instance.row->addChild(std::move(areaNode)));
      const float iconScale = model.active ? cfg.activeScale : cfg.inactiveScale;
      if (cfg.magnification) {
        Node* iconNode =
            item.iconImage != nullptr ? static_cast<Node*>(item.iconImage) : static_cast<Node*>(item.iconGlyph);
        if (iconNode != nullptr) {
          item.visualScale = iconScale;
          item.hoverMainOffset = 0.0F;
          const float badgeSize = std::max(kBadgeMinSize, iSize * kBadgeSizeRatio);
          applyHoverItemVisual(iconNode, item.badge, edge, kCellPad, kCellPad, iSize, badgeSize, iconScale);
        }
      }
    }

    if (launcherPosition == DockLauncherPosition::End) {
      instance.row->addChild(createLauncherButton(instance, cfg, renderer, clickContext));
    }

    if (instance.hasTrash) {
      instance.separators.push_back({.node = instance.row->addChild(makeDockSeparator(vert, cfg.scale))});
      instance.row->addChild(createTrashButton(instance, cfg, deps, clickContext));
    }

    if (cfg.magnification && instance.trash.icon != nullptr) {
      instance.trash.visualScale = cfg.inactiveScale;
      applyHoverItemVisual(
          instance.trash.icon, nullptr, edge, glyphBaseX(cfg), glyphBaseY(cfg), glyphCell(cfg), 0.0F,
          cfg.inactiveScale
      );
    }

    if (cfg.magnification && instance.launcherIconNode != nullptr) {
      const float launcherScale = cfg.inactiveScale;
      instance.launcherVisualScale = launcherScale;
      applyHoverItemVisual(
          instance.launcherIconNode, nullptr, edge, launcherIconBaseX(cfg), launcherIconBaseY(cfg),
          launcherIconSize(cfg), 0.0F, launcherScale
      );
    }

    shell::dock::resizeSurface(instance, cfg, deps.model.config.config().shell.shadow);

    if (cfg.magnification && instance.surface != nullptr) {
      instance.surface->requestFrameTick();
      instance.surface->requestRedraw();
    }
  }

  void updateVisuals(DockInstance& instance, DockItemSceneDependencies deps, const DockSnapshot& snapshot) {
    if (instance.surface == nullptr) {
      return;
    }
    syncDockLens(instance, snapshot);
    Renderer& renderer = instance.surface->renderTarget().renderer();

    const auto& cfg = instance.config;
    const auto& shell = deps.model.config.config().shell;
    const DockEdge edge = cfg.position;
    const std::size_t itemCount = std::min(instance.items.size(), snapshot.items.size());

    for (std::size_t itemIndex = 0; itemIndex < itemCount; ++itemIndex) {
      auto& item = instance.items[itemIndex];
      const auto& model = snapshot.items[itemIndex];
      const bool dragActive = instance.drag.active();
      const bool isDraggedItem = dragActive && itemIndex == instance.drag.sourceIndex;
      const float iconScale = model.active ? cfg.activeScale : cfg.inactiveScale;
      const float iconOpacity = model.active ? cfg.activeOpacity : cfg.inactiveOpacity;
      applyShellAppIconColorization(item.iconImage, shell);
      Node* iconNode =
          item.iconImage != nullptr ? static_cast<Node*>(item.iconImage) : static_cast<Node*>(item.iconGlyph);

      if (iconNode != nullptr) {
        if (!cfg.magnification) {
          if (!isDraggedItem) {
            iconNode->setPosition(kCellPad, kCellPad);
          }
          if (item.visualScale < 0.0F) {
            item.visualScale = iconScale;
            iconNode->setScale(iconScale);
          } else if (!isDraggedItem && std::abs(item.visualScale - iconScale) > 0.001F) {
            applyAnimatedIconScale(instance, iconNode, item.visualScale, item.scaleAnimId, iconScale);
          }
        }

        if (item.badge != nullptr && !cfg.magnification) {
          const float bd = std::max(kBadgeMinSize, static_cast<float>(cfg.iconSize) * kBadgeSizeRatio);
          applyBadgeVisual(item.badge, edge, kCellPad, kCellPad, static_cast<float>(cfg.iconSize), bd, 1.0F);
        }

        if (!cfg.magnification && !dragActive) {
          item.hoverMainOffset = 0.0F;
          applyItemMainOffset(item.area, shell::dock::isVerticalEdge(edge), item.glideMainOffset);
          if (item.area != nullptr) {
            item.area->setZIndex(0);
          }
        }

        if (!isDraggedItem) {
          if (item.visualOpacity < 0.0F) {
            item.visualOpacity = iconOpacity;
            iconNode->setOpacity(iconOpacity);
          } else if (std::abs(item.visualOpacity - iconOpacity) > 0.001F) {
            if (item.opacityAnimId != 0) {
              instance.animations.cancel(item.opacityAnimId);
            }
            item.opacityAnimId = instance.animations.animateSpatial(
                item.visualOpacity, iconOpacity,
                Style::spring(Tokens::springFade),
                [node = iconNode, itemPtr = &item](float value) {
                  itemPtr->visualOpacity = value;
                  node->setOpacity(value);
                },
                [itemPtr = &item] { itemPtr->opacityAnimId = 0; }
            );
          }
        }
      }

      const std::size_t count = model.instanceCount;
      if (item.area != nullptr) {
        const std::string tooltipText = dockItemTooltipText(model);
        if (item.tooltipText != tooltipText) {
          item.tooltipText = tooltipText;
          setDockTooltipText(*item.area, item.tooltipText);
        }
      }
      if (item.runningIndicator != nullptr) {
        const bool runningNow = model.running && !model.active;
        item.runningIndicator->setVisible(runningNow || item.runningScale > 0.001F);
        if (runningNow != item.runningShown) {
          item.runningShown = runningNow;
          if (item.runningAnimId != 0) {
            instance.animations.cancel(item.runningAnimId);
          }
          // The dot grows from its center on the effects spring.
          item.runningAnimId = instance.animations.animateSpatial(
              item.runningScale, runningNow ? 1.0F : 0.0F,
              Style::spring(Tokens::springFade),
              [&instance, itemPtr = &item, vert = shell::dock::isVerticalEdge(cfg.position)](float v) {
                itemPtr->runningScale = v;
                Node* bar = itemPtr->runningIndicator;
                bar->setTransformOrigin(bar->width() * 0.5F, bar->height() * 0.5F);
                if (vert) {
                  bar->setScale(1.0F, std::max(v, 0.0F));
                } else {
                  bar->setScale(std::max(v, 0.0F), 1.0F);
                }
                bar->setVisible(v > 0.001F);
                instance.surface->requestRedraw();
              },
              [itemPtr = &item] { itemPtr->runningAnimId = 0; }, item.runningIndicator
          );
        }
      }

      if (cfg.showDots) {
        const std::size_t dotCount = std::min<std::size_t>(count, 3);
        const auto iSize = static_cast<float>(cfg.iconSize);
        const float cellMain = iSize + 2.0F * kCellPad;
        const float dot = std::max(kDotMinSize * cfg.scale, std::round(iSize * kDotSizeRatio));
        const float groupLength =
            dotCount == 0 ? dot : dot * static_cast<float>(dotCount) + kDotGap * cfg.scale * static_cast<float>(dotCount - 1);
        const float groupStart = std::round((cellMain - groupLength) * 0.5F);
        const bool verticalDots = shell::dock::isVerticalEdge(edge);

        for (std::size_t dotIndex = 0; dotIndex < item.dotIndicators.size(); ++dotIndex) {
          if (item.dotIndicators[dotIndex] == nullptr) {
            continue;
          }
          Box* dotNode = item.dotIndicators[dotIndex];
          const bool visible = dotIndex < dotCount;
          dotNode->setVisible(visible);
          dotNode->setFill(colorSpecFromRole(model.active ? ColorRole::Accent : ColorRole::Accent));
          if (visible) {
            const float main = groupStart + static_cast<float>(dotIndex) * (dot + kDotGap * cfg.scale);
            if (verticalDots) {
              const float x = edge == DockEdge::Left ? 1.0F : std::round(cellMain - dot - 1.0F);
              dotNode->setPosition(x, main);
            } else {
              const float y = edge == DockEdge::Bottom ? std::round(cellMain - dot - 1.0F) : 1.0F;
              dotNode->setPosition(main, y);
            }
          }
        }
      }

      if (item.badge != nullptr && item.badgeLabel != nullptr) {
        // Unread notifications take the badge before the window count.
        const bool unread = model.unreadCount > 0;
        const std::size_t shown = unread ? model.unreadCount : count;
        const bool show = unread || (cfg.showInstanceCount && count >= 2);
        item.badge->setVisible(show);
        item.badgeLabel->setVisible(show);
        if (show) {
          item.badgeLabel->setColor(
              unread ? fixedColorSpec(Color{.r = 1.0F, .g = 1.0F, .b = 1.0F}) : colorSpecFromRole(ColorRole::OnAccent)
          );
          item.badge->setFill(colorSpecFromRole(unread ? ColorRole::Danger : ColorRole::Accent));
          const float bd = std::max(kBadgeMinSize, static_cast<float>(cfg.iconSize) * kBadgeSizeRatio);
          const float previousRight = item.badge->x() + item.badge->width();
          const float badgeW = count_badge::layout(renderer, *item.badge, *item.badgeLabel, shown, bd, 1.0F);
          if (edge != DockEdge::Right) {
            item.badge->setPosition(previousRight - badgeW, item.badge->y());
          }
        }
      }
    }

    if (instance.drag.active()) {
      applyDragVisuals(instance, cfg);
    }

    if (!cfg.magnification && instance.trash.icon != nullptr) {
      instance.trash.icon->setPosition(glyphBaseX(cfg), glyphBaseY(cfg));
      if (instance.trash.visualScale < 0.0F) {
        instance.trash.visualScale = cfg.inactiveScale;
        instance.trash.icon->setScale(cfg.inactiveScale);
      } else if (std::abs(instance.trash.visualScale - cfg.inactiveScale) > 0.001F) {
        applyAnimatedIconScale(
            instance, instance.trash.icon, instance.trash.visualScale, instance.trash.scaleAnimId, cfg.inactiveScale
        );
      }
      instance.trash.hoverMainOffset = 0.0F;
    }

    if (!cfg.magnification && instance.launcherIconNode != nullptr) {
      const float iconScale = cfg.inactiveScale;
      instance.launcherIconNode->setPosition(launcherIconBaseX(cfg), launcherIconBaseY(cfg));
      if (instance.launcherVisualScale < 0.0F) {
        instance.launcherVisualScale = iconScale;
        instance.launcherIconNode->setScale(iconScale);
      } else if (std::abs(instance.launcherVisualScale - iconScale) > 0.001F) {
        applyAnimatedIconScale(
            instance, instance.launcherIconNode, instance.launcherVisualScale, instance.launcherScaleAnimId, iconScale
        );
      }
      instance.launcherHoverMainOffset = 0.0F;
      if (instance.launcherArea != nullptr) {
        instance.launcherArea->setZIndex(0);
      }
    }
    if (!cfg.magnification) {
      applyDockOverflowEnds(instance, cfg);
    }

    // Magnification owns active/inactive scale via updateHoverZoom; kick it on focus updates.
    if (cfg.magnification
        && instance.surface != nullptr
        && updateHoverZoom(instance, snapshot, kHoverZoomReferenceFrameMs)) {
      instance.surface->requestFrameTick();
      instance.surface->requestRedraw();
    }
  }

  namespace {
  // A dock that scrolls keeps its caps (launcher, trash and its separator) at the pill ends by a pin translation; the
  // apps scroll in the span between them.
  struct OverflowEnds {
    bool active = false;
    float startPin = 0.0F;
    float endPin = 0.0F;
    float lo = 0.0F; // visible app span in row coordinates
    float hi = 0.0F;
    float endCapStart = std::numeric_limits<float>::infinity(); // rest main position of the first end cap
  };

  [[nodiscard]] OverflowEnds overflowEnds(const DockInstance& instance, const DockConfig& cfg) {
    OverflowEnds ends;
    if (instance.maxScrollOffset <= 0.0F || instance.viewport == nullptr) {
      return ends;
    }
    const bool vertical = shell::dock::isVerticalEdge(cfg.position);
    const auto spacing = static_cast<float>(cfg.itemSpacing);
    const auto mainPad = static_cast<float>(cfg.mainAxisPadding);
    const float viewLength = vertical ? instance.viewport->height() : instance.viewport->width();
    ends.active = true;
    ends.startPin = instance.scrollOffset;
    ends.endPin = instance.scrollOffset - instance.maxScrollOffset;
    if (cfg.launcherPosition == DockLauncherPosition::End && instance.launcherArea != nullptr) {
      ends.endCapStart = instance.launcherRestMainPos;
    }
    if (instance.trash.area != nullptr && !instance.separators.empty()) {
      ends.endCapStart = std::min(ends.endCapStart, instance.separators.back().restMainPos);
    }
    ends.lo = cfg.launcherPosition == DockLauncherPosition::Start && instance.launcherArea != nullptr
                  ? instance.launcherRestMainPos + ends.startPin + launcherIconSize(cfg) + spacing
                  : instance.scrollOffset + mainPad;
    ends.hi = std::isfinite(ends.endCapStart) ? ends.endCapStart + ends.endPin - spacing
                                              : instance.scrollOffset + viewLength - mainPad;
    return ends;
  }

  [[nodiscard]] bool isEndCapPos(const OverflowEnds& ends, float restMainPos) {
    return ends.active && restMainPos >= ends.endCapStart - 0.5F;
  }
  } // namespace

  void clearHoverZoom(DockInstance& instance, const DockSnapshot& snapshot) {
    instance.hoverPointerValid = false;
    if (instance.surface == nullptr) {
      return;
    }
    (void)updateHoverZoom(instance, snapshot, kHoverZoomReferenceFrameMs);
    instance.surface->requestFrameTick();
    instance.surface->requestRedraw();
  }

  bool syncHoverPointerFromScene(DockInstance& instance, const DockConfig& cfg, float sceneX, float sceneY) {
    if (instance.row == nullptr) {
      instance.hoverPointerValid = false;
      return false;
    }

    float localX = 0.0F;
    float localY = 0.0F;
    if (!Node::mapFromScene(instance.row, sceneX, sceneY, localX, localY)) {
      instance.hoverPointerValid = false;
      return false;
    }

    const bool vertical = shell::dock::isVerticalEdge(cfg.position);
    const float cellMain = static_cast<float>(cfg.iconSize) + 2.0F * kCellPad;
    const float itemPitch = cellMain + static_cast<float>(cfg.itemSpacing);
    const float mainPad = itemPitch * kHoverZoomInfluence;
    const float crossPad = static_cast<float>(shell::dock::dockHoverZoomCrossPad(cfg)) + kCellPad;
    const float main = vertical ? localY : localX;
    const float cross = vertical ? localX : localY;
    const float mainSize = vertical ? instance.row->height() : instance.row->width();
    const float crossSize = vertical ? instance.row->width() : instance.row->height();
    // Between the icons and the screen edge the pointer is still on the dock: only the inner side ends the hover.
    const bool edgeAtEnd = cfg.position == DockEdge::Bottom || cfg.position == DockEdge::Right;
    const bool pastInnerSide = edgeAtEnd ? cross < -crossPad : cross > crossSize + crossPad;
    if (main < -mainPad || main > mainSize + mainPad || pastInnerSide) {
      instance.hoverPointerValid = false;
      return false;
    }

    instance.hoverPointerMain = main;
    instance.hoverPointerValid = true;
    return true;
  }

  bool updateHoverZoom(DockInstance& instance, const DockSnapshot& snapshot, float deltaMs, bool relaid) {
    // Zoom moves and resizes nodes it owns; outside a phase each resize would request a rebuild that resets the zoom.
    UiPhaseScope zoomPhase(UiPhase::Update);
    const auto& cfg = instance.config;
    if (!cfg.magnification || instance.row == nullptr) {
      return false;
    }
    if (instance.drag.active()) {
      applyDragVisuals(instance, cfg);
      return false;
    }

    const float lerpFactor = hoverZoomFrameLerp(deltaMs);
    const DockEdge edge = cfg.position;
    const bool vertical = shell::dock::isVerticalEdge(edge);
    const auto iSize = static_cast<float>(cfg.iconSize);
    const float cellMain = iSize + 2.0F * kCellPad;
    const float itemPitch = cellMain + static_cast<float>(cfg.itemSpacing);
    const float badgeSize = std::max(kBadgeMinSize, iSize * kBadgeSizeRatio);
    const float baseLauncherScale = cfg.inactiveScale;
    const bool pointerActive = instance.pointerInside && instance.hoverPointerValid;
    const std::size_t itemCount = std::min(instance.items.size(), snapshot.items.size());
    bool needsMoreFrames = false;
    const float easeStep = std::clamp(deltaMs, 1.0F, 50.0F) / kHoverZoomEaseMs;
    instance.hoverAmount = std::clamp(instance.hoverAmount + (pointerActive ? easeStep : -easeStep), 0.0F, 1.0F);
    // Inside the dock only the pointer moves the zoom; the amount eases it in and out at the dock's edges.
    const float amount = instance.hoverAmount * instance.hoverAmount * (3.0F - 2.0F * instance.hoverAmount);
    const bool zoomActive = instance.hoverAmount > 0.0F;
    if (zoomActive && instance.hoverAmount < 1.0F) {
      needsMoreFrames = true;
    }

    struct HoverSlot {
      InputArea* area = nullptr;
      Box* badge = nullptr;
      Node* iconNode = nullptr;
      float* visualScale = nullptr;
      float* hoverMainOffset = nullptr;
      AnimationManager::Id* scaleAnimId = nullptr;
      float restMainPos = 0.0F;
      float glideOffset = 0.0F;
      float baseScale = 1.0F;
      float targetScale = 1.0F;
      float targetMainOffset = 0.0F;
      float restCenterMain = 0.0F;
      float iconBaseX = kCellPad;
      float iconBaseY = kCellPad;
      float size = 0.0F; // rest main-axis size of the icon and its cell
      float pin = 0.0F;  // overflow pin of a cap; restMainPos includes it
      bool cap = false;
    };
    const OverflowEnds ends = overflowEnds(instance, cfg);
    const float launcherPin = cfg.launcherPosition == DockLauncherPosition::Start ? ends.startPin : ends.endPin;

    std::vector<HoverSlot> slots;
    slots.reserve(itemCount + 1U);
    if (cfg.launcherPosition == DockLauncherPosition::Start && instance.launcherArea != nullptr) {
      slots.push_back(
          HoverSlot{
              .area = instance.launcherArea,
              .iconNode = instance.launcherIconNode,
              .visualScale = &instance.launcherVisualScale,
              .hoverMainOffset = &instance.launcherHoverMainOffset,
              .scaleAnimId = &instance.launcherScaleAnimId,
              .restMainPos = instance.launcherRestMainPos + launcherPin,
              .baseScale = baseLauncherScale,
              .restCenterMain = itemRestCenterMain(instance.launcherRestMainPos + launcherPin, launcherIconSize(cfg)),
              .iconBaseX = launcherIconBaseX(cfg),
              .iconBaseY = launcherIconBaseY(cfg),
              .size = launcherIconSize(cfg),
              .pin = launcherPin,
              .cap = true,
          }
      );
    }
    for (std::size_t itemIndex = 0; itemIndex < itemCount; ++itemIndex) {
      auto& item = instance.items[itemIndex];
      const auto& model = snapshot.items[itemIndex];
      // Magnification owns the position from here: a glide still running would fight it each frame.
      if (pointerActive && item.glideAnimId != 0) {
        instance.animations.cancel(item.glideAnimId);
        item.glideAnimId = 0;
        item.glideMainOffset = 0.0F;
      }
      Node* iconNode =
          item.iconImage != nullptr ? static_cast<Node*>(item.iconImage) : static_cast<Node*>(item.iconGlyph);
      slots.push_back(
          HoverSlot{
              .area = item.area,
              .badge = item.badge,
              .iconNode = iconNode,
              .visualScale = &item.visualScale,
              .hoverMainOffset = &item.hoverMainOffset,
              .scaleAnimId = &item.scaleAnimId,
              .restMainPos = item.restMainPos + item.glideMainOffset,
              .glideOffset = item.glideMainOffset,
              .baseScale = model.active ? cfg.activeScale : cfg.inactiveScale,
              .restCenterMain = itemRestCenterMain(item.restMainPos, cellMain),
              .size = iSize,
          }
      );
    }
    if (cfg.launcherPosition == DockLauncherPosition::End && instance.launcherArea != nullptr) {
      slots.push_back(
          HoverSlot{
              .area = instance.launcherArea,
              .iconNode = instance.launcherIconNode,
              .visualScale = &instance.launcherVisualScale,
              .hoverMainOffset = &instance.launcherHoverMainOffset,
              .scaleAnimId = &instance.launcherScaleAnimId,
              .restMainPos = instance.launcherRestMainPos + launcherPin,
              .baseScale = baseLauncherScale,
              .restCenterMain = itemRestCenterMain(instance.launcherRestMainPos + launcherPin, launcherIconSize(cfg)),
              .iconBaseX = launcherIconBaseX(cfg),
              .iconBaseY = launcherIconBaseY(cfg),
              .size = launcherIconSize(cfg),
              .pin = launcherPin,
              .cap = true,
          }
      );
    }

    if (instance.trash.area != nullptr) {
      slots.push_back(
          HoverSlot{
              .area = instance.trash.area,
              .iconNode = instance.trash.icon,
              .visualScale = &instance.trash.visualScale,
              .hoverMainOffset = &instance.trash.hoverMainOffset,
              .scaleAnimId = &instance.trash.scaleAnimId,
              .restMainPos = instance.trash.restMainPos + ends.endPin,
              .baseScale = baseLauncherScale,
              .restCenterMain = itemRestCenterMain(instance.trash.restMainPos + ends.endPin, glyphCell(cfg)),
              .iconBaseX = glyphBaseX(cfg),
              .iconBaseY = glyphBaseY(cfg),
              .size = glyphCell(cfg),
              .pin = ends.endPin,
              .cap = true,
          }
      );
    }

    std::vector<float> slotScales;
    slotScales.reserve(slots.size());
    for (HoverSlot& slot : slots) {
      const float fullMultiplier = zoomActive ? computeHoverMultiplier(
                                                   instance.hoverPointerMain, slot.restCenterMain, itemPitch,
                                                   cfg.magnificationScale, kHoverZoomFalloffInfluence
                                               )
                                             : 1.0F;
      const float hoverMultiplier = 1.0F + (fullMultiplier - 1.0F) * amount;
      slot.targetScale = slot.baseScale * hoverMultiplier;
      slotScales.push_back(slot.targetScale);
    }

    std::vector<float> targetOffsets;
    if (zoomActive && ends.active) {
      // The caps stay pinned; the apps whose cells lie inside the span spread, then move as one so every grown icon
      // stays inside it. Cells already part-hidden move with them and fade further.
      targetOffsets.assign(slots.size(), 0.0F);
      std::vector<std::size_t> spread;
      std::vector<float> scales;
      std::vector<float> sizes;
      for (std::size_t index = 0; index < slots.size(); ++index) {
        const HoverSlot& slot = slots[index];
        if (!slot.cap && slot.restMainPos + slot.size > ends.lo && slot.restMainPos < ends.hi) {
          spread.push_back(index);
          scales.push_back(slotScales[index]);
          sizes.push_back(slot.size);
        }
      }
      const std::vector<float> offsets = shell::dock::hoverSpreadOffsets(scales, sizes);
      // A grown cap takes room from the span on its side.
      float lo = ends.lo;
      float hi = ends.hi;
      for (std::size_t index = 0; index < slots.size(); ++index) {
        const HoverSlot& slot = slots[index];
        if (slot.cap) {
          const float growth = slot.size * std::max(0.0F, slotScales[index] - 1.0F) * 0.5F;
          if (slot.restMainPos < ends.lo) {
            lo = std::max(lo, ends.lo + growth);
          } else {
            hi = std::min(hi, ends.hi - growth);
          }
        }
      }
      const float zoomRange = std::max(0.001F, cfg.magnificationScale - 1.0F);
      // Drawn extent of the spread apps with their offsets times `factor`. A cell part-hidden at an end counts as it
      // grows, so the row reveals a hovered end icon smoothly.
      const auto extent = [&](float factor) {
        float low = std::numeric_limits<float>::infinity();
        float high = -std::numeric_limits<float>::infinity();
        for (std::size_t k = 0; k < spread.size(); ++k) {
          const HoverSlot& slot = slots[spread[k]];
          const float center = slot.restMainPos + slot.size * 0.5F + offsets[k] * factor;
          const float weight =
              slot.restMainPos < ends.lo - 0.5F || slot.restMainPos + slot.size > ends.hi + 0.5F
                  ? std::clamp((scales[k] / slot.baseScale - 1.0F) / zoomRange, 0.0F, 1.0F)
                  : 1.0F;
          low = std::min(low, lo + (center - slot.size * scales[k] * 0.5F - lo) * weight);
          high = std::max(high, hi + (center + slot.size * scales[k] * 0.5F - hi) * weight);
        }
        return std::pair{low, high};
      };
      float factor = 1.0F;
      if (const auto [fullLow, fullHigh] = extent(1.0F); fullHigh - fullLow > hi - lo) {
        float fits = 0.0F;
        float overflows = 1.0F;
        for (int step = 0; step < 12; ++step) {
          const float mid = (fits + overflows) * 0.5F;
          const auto [l, h] = extent(mid);
          (h - l > hi - lo ? overflows : fits) = mid;
        }
        factor = fits;
      }
      const auto [low, high] = extent(factor);
      float shift = 0.0F;
      if (high > hi) {
        shift = hi - high;
      } else if (low < lo) {
        shift = lo - low;
      }
      for (std::size_t k = 0; k < spread.size(); ++k) {
        targetOffsets[spread[k]] = offsets[k] * factor + shift;
      }
    } else if (zoomActive) {
      std::vector<float> restMainPos;
      std::vector<float> sizes;
      restMainPos.reserve(slots.size());
      sizes.reserve(slots.size());
      for (const HoverSlot& slot : slots) {
        restMainPos.push_back(slot.restMainPos);
        sizes.push_back(slot.size);
      }
      targetOffsets = shell::dock::hoverSpreadOffsets(slotScales, sizes);
      const float rowMainSize = vertical ? instance.row->height() : instance.row->width();
      const auto mainPad = static_cast<float>(cfg.mainAxisPadding);
      // A dock that fits grows its pill into the surface's zoom room; a scrolling dock spreads inside its row.
      const float growRoom =
          instance.maxScrollOffset > 0.0F ? 0.0F : static_cast<float>(shell::dock::dockHoverZoomMainPad(cfg));
      clampSpreadOffsetsToBounds(
          restMainPos, slotScales, sizes, mainPad - growRoom, rowMainSize - mainPad + growRoom, targetOffsets
      );
    } else {
      targetOffsets.assign(slots.size(), 0.0F);
    }

    for (std::size_t index = 0; index < slots.size(); ++index) {
      slots[index].targetMainOffset = targetOffsets[index];
    }

    for (HoverSlot& slot : slots) {
      if (lerpHoverMainOffset(*slot.hoverMainOffset, zoomActive ? slot.targetMainOffset : 0.0F, lerpFactor)) {
        needsMoreFrames = true;
      }
      applyItemMainOffset(slot.area, vertical, slot.pin + slot.glideOffset + *slot.hoverMainOffset);

      if (applyHoverIconScale(
              slot.iconNode, *slot.visualScale, *slot.scaleAnimId, instance, slot.targetScale, slot.baseScale,
              lerpFactor, edge, slot.iconBaseX, slot.iconBaseY, slot.size, slot.badge, badgeSize, relaid
          )) {
        needsMoreFrames = true;
      }

      const float paintScale = *slot.visualScale > 0.0F ? *slot.visualScale : slot.targetScale;
      applyHoverZoomZIndex(slot.area, paintScale);
      if (!instance.drag.active()) {
        syncHoveredTooltipAnchor(slot.area, cfg, slot.iconBaseX, slot.iconBaseY, slot.size, paintScale);
      }
    }

    // Drawn main extent of a slot's icon: live (current offset and scale) or at rest.
    const auto slotEdge = [&](const HoverSlot& slot, bool live, float side) {
      const float scale = !live ? slot.baseScale : (*slot.visualScale > 0.0F ? *slot.visualScale : slot.targetScale);
      return slot.restMainPos + slot.size * 0.5F + (live ? *slot.hoverMainOffset : 0.0F) +
             side * slot.size * scale * 0.5F;
    };
    for (const auto& separator : instance.separators) {
      if (isEndCapPos(ends, separator.restMainPos)) {
        applyItemMainOffset(separator.node, vertical, ends.endPin);
        continue;
      }
      const HoverSlot* before = nullptr;
      const HoverSlot* after = nullptr;
      for (const HoverSlot& slot : slots) {
        if (ends.active && slot.cap) {
          continue;
        }
        if (slot.restMainPos < separator.restMainPos && (before == nullptr || slot.restMainPos > before->restMainPos)) {
          before = &slot;
        } else if (slot.restMainPos > separator.restMainPos &&
                   (after == nullptr || slot.restMainPos < after->restMainPos)) {
          after = &slot;
        }
      }
      float offset = 0.0F;
      if (before != nullptr && after != nullptr) {
        offset = std::round(
            (slotEdge(*before, true, 1.0F) + slotEdge(*after, true, -1.0F) - slotEdge(*before, false, 1.0F) -
             slotEdge(*after, false, -1.0F)) *
            0.5F
        );
      }
      applyItemMainOffset(separator.node, vertical, offset);
    }

    // The pill follows the drawn icons past its rest ends, so a magnified end icon never leaves the glass.
    if (instance.maxScrollOffset <= 0.0F) {
      const float rowMainSize = vertical ? instance.row->height() : instance.row->width();
      const auto mainPad = static_cast<float>(cfg.mainAxisPadding);
      float low = mainPad;
      float high = rowMainSize - mainPad;
      for (const HoverSlot& slot : slots) {
        const float scale = *slot.visualScale > 0.0F ? *slot.visualScale : slot.targetScale;
        const float center = slot.restMainPos + slot.size * 0.5F + *slot.hoverMainOffset;
        low = std::min(low, center - slot.size * scale * 0.5F);
        high = std::max(high, center + slot.size * scale * 0.5F);
      }
      shell::dock::applyDockZoomGrow(
          instance, cfg, std::round(mainPad - low), std::round(high - (rowMainSize - mainPad))
      );
    }

    // The lens grows with the magnified focused icon: the icon's drawn rect plus the lens inset.
    if (instance.lensActiveIndex < instance.items.size() && !instance.drag.active()) {
      const auto& lensItem = instance.items[instance.lensActiveIndex];
      const InputArea* area = lensItem.area;
      if (area != nullptr && lensItem.visualScale > 0.0F) {
        const TooltipAnchorInsets in = scaledIconTooltipAnchor(area, edge, kCellPad, kCellPad, iSize, lensItem.visualScale);
        const float k = iSize / shell::dock::designIconSize();
        const float drawn = vertical ? area->height() - in.top - in.bottom : area->width() - in.left - in.right;
        const float grow = drawn / iSize;
        const float inset = Tokens::componentDockLensInset * k * grow;
        const float mainStart =
            vertical ? area->y() + area->translationY() + in.top : area->x() + area->translationX() + in.left;
        // Only the main axis grows: the dock does not thicken, so the magnified icon rises past a lens kept inside it.
        const float restInset = Tokens::componentDockLensInset * k;
        instance.lens.moveTo(
            mainStart - inset, mainStart + drawn + inset, lensItem.restCrossPos - restInset, iSize + 2.0F * restInset,
            Style::scaledRadius(Tokens::componentDockLensRadius, k), false
        );
      }
    }

    if (ends.active) {
      applyDockOverflowEnds(instance, cfg);
    }
    if (needsMoreFrames && instance.sceneRoot != nullptr) {
      instance.sceneRoot->markPaintDirty();
    }
    return needsMoreFrames;
  }

  void applyDockOverflowEnds(DockInstance& instance, const DockConfig& cfg) {
    if (instance.row == nullptr) {
      return;
    }
    const bool vertical = shell::dock::isVerticalEdge(cfg.position);
    const OverflowEnds ends = overflowEnds(instance, cfg);
    const float launcherPin = cfg.launcherPosition == DockLauncherPosition::Start ? ends.startPin : ends.endPin;
    applyItemMainOffset(instance.launcherArea, vertical, launcherPin + instance.launcherHoverMainOffset);
    applyItemMainOffset(instance.trash.area, vertical, ends.endPin + instance.trash.hoverMainOffset);
    if (ends.active) {
      for (InputArea* cap : {instance.launcherArea, instance.trash.area}) {
        if (cap != nullptr) {
          cap->setZIndex(kPinnedCapZ);
        }
      }
    }
    for (const auto& separator : instance.separators) {
      if (isEndCapPos(ends, separator.restMainPos)) {
        applyItemMainOffset(separator.node, vertical, ends.endPin);
      }
    }
    const auto cell = static_cast<float>(cfg.iconSize) + 2.0F * kCellPad;
    for (auto& item : instance.items) {
      if (item.area == nullptr || item.enterAnimId != 0) {
        continue;
      }
      float opacity = 1.0F;
      if (ends.active) {
        const float start = item.restMainPos + item.glideMainOffset + item.hoverMainOffset;
        const float shown = std::clamp(std::min(start + cell, ends.hi) - std::max(start, ends.lo), 0.0F, cell) / cell;
        // Gone by half-hidden, so a cell's ink never draws over a cap's glyph.
        const float fade = std::clamp(shown * 2.0F - 1.0F, 0.0F, 1.0F);
        opacity = fade * fade;
      }
      if (std::abs(item.area->opacity() - opacity) > 0.001F) {
        item.area->setOpacity(opacity);
        item.area->setHitTestVisible(opacity > 0.0F);
      }
    }
    if (instance.sceneRoot != nullptr) {
      instance.sceneRoot->markPaintDirty();
    }
  }

  void syncDockItemRestPositions(DockInstance& instance, const DockConfig& cfg) {
    if (instance.row == nullptr) {
      return;
    }
    const bool vertical = shell::dock::isVerticalEdge(cfg.position);
    for (auto& separator : instance.separators) {
      separator.restMainPos = vertical ? separator.node->y() : separator.node->x();
    }
    if (instance.trash.area != nullptr) {
      instance.trash.restMainPos = vertical ? instance.trash.area->y() : instance.trash.area->x();
      if (!instance.hoverPointerValid) {
        instance.trash.hoverMainOffset = 0.0F;
      }
    }
    if (cfg.launcherPosition == DockLauncherPosition::Start && instance.launcherArea != nullptr) {
      instance.launcherRestMainPos = vertical ? instance.launcherArea->y() : instance.launcherArea->x();
      if (!instance.hoverPointerValid) {
        instance.launcherHoverMainOffset = 0.0F;
      }
    }
    for (auto& item : instance.items) {
      if (item.area == nullptr) {
        continue;
      }
      item.restMainPos = vertical ? item.area->y() : item.area->x();
      item.restCrossPos = vertical ? item.area->x() : item.area->y();
      // A relayout under the pointer keeps the spread, so magnification continues instead of restarting from rest.
      if (!instance.hoverPointerValid) {
        item.hoverMainOffset = 0.0F;
      }
      const auto from = instance.glideFrom.find(item.idLower);
      if (from == instance.glideFrom.end() && !instance.glideFrom.empty() && !item.isDragGhost) {
        // An icon that joins the row (an app that runs without a pin) grows in while its neighbours glide apart.
        DockItemView* view = &item;
        auto enter = [view, &instance](float v) {
          view->area->setTransformOrigin(view->area->width() * 0.5F, view->area->height() * 0.5F);
          view->area->setScale(v);
          view->area->setOpacity(v);
          if (instance.sceneRoot != nullptr) {
            instance.sceneRoot->markPaintDirty();
          }
        };
        enter(0.0F);
        item.enterAnimId = instance.animations.animateSpring(
            0.0F, 1.0F, Style::spring(Tokens::springMove), enter, [view] { view->enterAnimId = 0; }, view
        );
        continue;
      }
      if (from == instance.glideFrom.end() || std::abs(from->second - item.restMainPos) < 0.5F) {
        continue;
      }
      // Icons that stay glide into their new places instead of jumping when an app opens or closes (motion audit 4).
      item.glideMainOffset = from->second - item.restMainPos;
      applyItemMainOffset(item.area, vertical, item.glideMainOffset);
      DockItemView* view = &item;
      item.glideAnimId = instance.animations.animateSpring(
          item.glideMainOffset, 0.0F, Style::spring(Tokens::springMove),
          [view, vertical, &instance](float offset) {
            view->glideMainOffset = offset;
            applyItemMainOffset(view->area, vertical, view->hoverMainOffset + view->dragMainOffset + offset);
            if (instance.sceneRoot != nullptr) {
              instance.sceneRoot->markPaintDirty();
            }
          },
          [view] { view->glideAnimId = 0; }, view, 0.0F, Unit::Px
      );
    }
    instance.glideFrom.clear();
    if (cfg.launcherPosition == DockLauncherPosition::End && instance.launcherArea != nullptr) {
      instance.launcherRestMainPos = vertical ? instance.launcherArea->y() : instance.launcherArea->x();
      instance.launcherHoverMainOffset = 0.0F;
    }
    applyDockOverflowEnds(instance, cfg);
  }

  void handleItemClick(DockInstance& instance, const DockItemAction& action, DockItemClickContext& context) {
    if (context.callbacks.activateOrLaunch) {
      context.callbacks.activateOrLaunch(instance, action);
    }
  }

  // ── Drag-to-reorder ──────────────────────────────────────────────────────────

  std::size_t computeDragTargetIndex(const DockInstance& instance, const DockConfig& cfg, float mainPos) {
    const std::size_t pinnedCount = cfg.pinned.size();
    if (pinnedCount == 0 || instance.items.empty()) {
      return 0;
    }
    const float cellMain = static_cast<float>(cfg.iconSize) + 2.0F * kCellPad;
    const float pitch = cellMain + static_cast<float>(cfg.itemSpacing);

    std::size_t target = 0;
    const std::size_t limit = std::min(pinnedCount, instance.items.size());
    for (std::size_t i = 0; i < limit; ++i) {
      const auto& item = instance.items[i];
      if (item.area == nullptr) {
        continue;
      }
      const float restCenter = item.restMainPos + cellMain * 0.5F;
      if (mainPos > restCenter + pitch * 0.05F) {
        target = i + 1;
      }
    }
    return std::min(target, pinnedCount);
  }

  void applyDragVisuals(DockInstance& instance, const DockConfig& cfg) {
    if (!instance.drag.active() || instance.items.empty()) {
      return;
    }
    const bool vertical = shell::dock::isVerticalEdge(cfg.position);
    const float cellMain = static_cast<float>(cfg.iconSize) + 2.0F * kCellPad;
    const float pitch = cellMain + static_cast<float>(cfg.itemSpacing);
    const std::size_t pinnedCount = cfg.pinned.size();
    const std::size_t source = instance.drag.sourceIndex;
    const std::size_t target = instance.drag.targetIndex;

    for (std::size_t i = 0; i < instance.items.size(); ++i) {
      auto& item = instance.items[i];
      if (item.area == nullptr) {
        continue;
      }

      float extraOffset = 0.0F;
      if (i == source) {
        extraOffset = instance.drag.currentMain - instance.drag.startMain;
        item.area->setZIndex(200);
        Node* iconNode =
            item.iconImage != nullptr ? static_cast<Node*>(item.iconImage) : static_cast<Node*>(item.iconGlyph);
        if (iconNode != nullptr) {
          iconNode->setOpacity(0.85F);
        }
      } else if (i < pinnedCount) {
        item.area->setZIndex(0);
        const bool before = (i < source && i >= target);
        const bool after = (i > source && i < target);
        if (before) {
          extraOffset = pitch;
        } else if (after) {
          extraOffset = -pitch;
        }
        Node* iconNode =
            item.iconImage != nullptr ? static_cast<Node*>(item.iconImage) : static_cast<Node*>(item.iconGlyph);
        if (iconNode != nullptr) {
          iconNode->setOpacity(item.visualOpacity >= 0.0F ? item.visualOpacity : 1.0F);
        }
      } else {
        item.area->setZIndex(0);
        Node* iconNode =
            item.iconImage != nullptr ? static_cast<Node*>(item.iconImage) : static_cast<Node*>(item.iconGlyph);
        if (iconNode != nullptr) {
          iconNode->setOpacity(item.visualOpacity >= 0.0F ? item.visualOpacity : 1.0F);
        }
      }

      item.dragMainOffset = extraOffset;
      applyItemMainOffset(item.area, vertical, item.hoverMainOffset + extraOffset + item.glideMainOffset);
    }
  }

  void clearDragVisuals(DockInstance& instance, const DockConfig& cfg) {
    const bool vertical = shell::dock::isVerticalEdge(cfg.position);
    for (auto& item : instance.items) {
      if (item.area == nullptr) {
        continue;
      }
      item.dragMainOffset = 0.0F;
      item.isDragGhost = false;
      item.area->setZIndex(0);
      applyItemMainOffset(item.area, vertical, item.hoverMainOffset + item.glideMainOffset);
      Node* iconNode =
          item.iconImage != nullptr ? static_cast<Node*>(item.iconImage) : static_cast<Node*>(item.iconGlyph);
      if (iconNode != nullptr) {
        const float opacity = item.visualOpacity >= 0.0F ? item.visualOpacity : 1.0F;
        iconNode->setOpacity(opacity);
      }
    }
  }

  void dismissDockTooltip() { dismissDockTooltipImpl(); }

} // namespace shell::dock
