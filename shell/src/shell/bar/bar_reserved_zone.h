#pragma once

#include "design_tokens.h"
#include "ui/surface_shadow.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

/// A bar with no surface (macOS layout): its ink sits on the wallpaper over the edge scrim.
[[nodiscard]] inline bool barIsBare(const BarConfig& barConfig) {
  return !barConfig.islands && barConfig.backgroundOpacity <= 0.0F;
}

/// Room the bar surface keeps past the bar: the shadow bleed, and on the inner side of a bare bar the scrim.
[[nodiscard]] inline ui::surface_shadow::Bleed
barSurfaceBleed(const BarConfig& barConfig, const ShellConfig::ShadowConfig& shadowConfig) {
  auto bleed = ui::surface_shadow::bleed(barConfig.shadow, shadowConfig);
  if (!barIsBare(barConfig)) {
    return bleed;
  }
  const auto reach = std::max<std::int32_t>(
      0, static_cast<std::int32_t>(std::lround(Tokens::componentBarScrimHeight)) - barConfig.thickness
  );
  std::int32_t& inner = barConfig.position == "bottom" ? bleed.up
      : barConfig.position == "left"                   ? bleed.right
      : barConfig.position == "right"                  ? bleed.left
                                                       : bleed.down;
  inner = std::max(inner, reach);
  return bleed;
}

/// Layer-shell exclusive zone the bar reserves on its anchored screen edge.
/// Canonical definition shared by the bar surface and attached panels so a panel
/// can anchor against the same reserved edge the compositor places the bar on.
[[nodiscard]] inline std::int32_t
reservedBarExclusiveZone(const BarConfig& barConfig, const ShellConfig::ShadowConfig& shadowConfig) {
  const std::int32_t mEdge = barConfig.marginEdge;
  const std::int32_t mOpposite = std::max<std::int32_t>(0, barConfig.marginOppositeEdge);
  // Auto-hide folds the edge margin into the surface as a gutter and sets the
  // layer margin to 0, so the compositor adds no margin to the exclusive zone.
  // The zone must then cover the full edge gap itself (thickness + edge margin);
  // the edge-side shadow is allowed to bleed beyond, matching the layer-margin path.
  if ((barConfig.autoHide || barConfig.smartAutoHide) && mEdge > 0) {
    return barConfig.thickness + mEdge + mOpposite;
  }
  const auto sb = barSurfaceBleed(barConfig, shadowConfig);
  if (barConfig.position == "bottom") {
    return barConfig.thickness + std::min(mEdge, sb.down) + mOpposite;
  }
  if (barConfig.position == "top") {
    return std::min(mEdge, sb.up) + barConfig.thickness + mOpposite;
  }
  if (barConfig.position == "right") {
    return barConfig.thickness + std::min(mEdge, sb.right) + mOpposite;
  }
  if (barConfig.position == "left") {
    return std::min(mEdge, sb.left) + barConfig.thickness + mOpposite;
  }
  return barConfig.thickness + mOpposite;
}

/// Layer-shell margin the bar applies on its anchored screen edge. The edge gap
/// (marginEdge) is split: the part covered by the surface's own edge-side shadow
/// bleed lives inside the surface, and the remainder becomes the layer margin.
/// Auto-hide folds the whole gap into the surface as a gutter, so the margin is 0.
[[nodiscard]] inline std::int32_t
barEdgeLayerMargin(const BarConfig& barConfig, const ShellConfig::ShadowConfig& shadowConfig) {
  const std::int32_t mEdge = std::max<std::int32_t>(0, barConfig.marginEdge);
  if ((barConfig.autoHide || barConfig.smartAutoHide) && mEdge > 0) {
    return 0;
  }
  const auto sb = barSurfaceBleed(barConfig, shadowConfig);
  if (barConfig.position == "bottom") {
    return std::max<std::int32_t>(0, mEdge - sb.down);
  }
  if (barConfig.position == "top") {
    return std::max<std::int32_t>(0, mEdge - sb.up);
  }
  if (barConfig.position == "right") {
    return std::max<std::int32_t>(0, mEdge - sb.right);
  }
  if (barConfig.position == "left") {
    return std::max<std::int32_t>(0, mEdge - sb.left);
  }
  return 0;
}

/// Total distance the compositor reserves from the screen edge for the bar:
/// the exclusive zone plus the layer margin. Surfaces that anchor against the
/// bar's edge with exclusive_zone = 0 must subtract this (not just the exclusive
/// zone) to track the bar's real inner edge — otherwise a floating bar whose
/// shadow bleed is smaller than its edge margin leaves the margin uncounted.
[[nodiscard]] inline std::int32_t
reservedBarEdgeDistance(const BarConfig& barConfig, const ShellConfig::ShadowConfig& shadowConfig) {
  return reservedBarExclusiveZone(barConfig, shadowConfig) + barEdgeLayerMargin(barConfig, shadowConfig);
}
