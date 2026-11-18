// A flush dock only overlaps the screen edge on a fractionally scaled output, where the
// compositor can round the edge a device pixel short. At an integer scale the overlap would
// clip the panel's own edge row, so the surface must sit exactly on the boundary.

#include "config/config_types.h"
#include "shell/dock/dock_geometry.h"
#include "tests/test_check.h"
#include "wayland/surface.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <random>
#include <vector>

namespace {

  constexpr std::size_t kItemCount = 4;

  DockConfig flushDock(DockEdge edge) {
    DockConfig cfg;
    cfg.position = edge;
    cfg.marginEdge = 0;
    cfg.shadow = false;
    return cfg;
  }

  shell::dock::DockSurfaceGeometry geometryFor(const DockConfig& cfg, bool fractionalScale) {
    const ShellConfig::ShadowConfig shadow;
    return shell::dock::computeSurfaceGeometry(cfg, shadow, kItemCount, fractionalScale, 1920, 1080);
  }

  int edgeMargin(const shell::dock::DockSurfaceGeometry& geometry, DockEdge edge) {
    switch (edge) {
    case DockEdge::Bottom:
      return geometry.marginBottom;
    case DockEdge::Top:
      return geometry.marginTop;
    case DockEdge::Left:
      return geometry.marginLeft;
    case DockEdge::Right:
      return geometry.marginRight;
    }
    return 0;
  }

  bool panelTouchesSurfaceEdge(
      const shell::dock::DockPanelGeometry& panel, const shell::dock::DockSurfaceGeometry& surface, DockEdge edge
  ) {
    switch (edge) {
    case DockEdge::Bottom:
      return panel.panelY + panel.panelH == static_cast<float>(surface.surfaceH);
    case DockEdge::Top:
      return panel.panelY == 0.0F;
    case DockEdge::Left:
      return panel.panelX == 0.0F;
    case DockEdge::Right:
      return panel.panelX + panel.panelW == static_cast<float>(surface.surfaceW);
    }
    return false;
  }

} // namespace

namespace {

  // Random hover scales (1 to 2.5, any shape): no two drawn icons come closer than at rest, and a tiny scale change
  // (the pointer moving one step, even across an icon boundary) moves no icon by more than a tiny amount.
  void checkHoverSpreadNeverOverlaps() {
    constexpr float kIcon = 48.0F;
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> scale(1.0F, 2.5F);
    std::uniform_int_distribution<std::size_t> count(1, 12);
    for (int run = 0; run < 2000; ++run) {
      std::vector<float> scales(count(rng));
      for (float& s : scales) {
        s = scale(rng);
      }
      const std::vector<float> offsets = shell::dock::hoverSpreadOffsets(scales, kIcon);
      TEST_CHECK(offsets.size() == scales.size());
      constexpr float kStep = 0.001F;
      std::vector<float> nudged = scales;
      nudged[run % nudged.size()] += kStep;
      const std::vector<float> moved = shell::dock::hoverSpreadOffsets(nudged, kIcon);
      for (std::size_t i = 0; i < scales.size(); ++i) {
        TEST_CHECK(std::abs(moved[i] - offsets[i]) <= kIcon * kStep * static_cast<float>(scales.size()));
      }
      for (std::size_t i = 1; i < scales.size(); ++i) {
        const float shrink = offsets[i] - offsets[i - 1] - kIcon * (scales[i - 1] + scales[i] - 2.0F) * 0.5F;
        TEST_CHECK(shrink >= -0.01F);
      }
    }
    // The pointer halfway between two icons: either one may be the larger by a hair.
    const std::vector<float> leftAhead = shell::dock::hoverSpreadOffsets({1.0F, 1.801F, 1.8F, 1.0F}, kIcon);
    const std::vector<float> rightAhead = shell::dock::hoverSpreadOffsets({1.0F, 1.8F, 1.801F, 1.0F}, kIcon);
    for (std::size_t i = 0; i < leftAhead.size(); ++i) {
      TEST_CHECK(std::abs(leftAhead[i] - rightAhead[i]) <= 0.5F);
    }
  }

} // namespace

int main() {
  checkHoverSpreadNeverOverlaps();
  constexpr DockEdge kEdges[] = {DockEdge::Bottom, DockEdge::Top, DockEdge::Left, DockEdge::Right};

  for (const DockEdge edge : kEdges) {
    const DockConfig cfg = flushDock(edge);
    TEST_CHECK(edgeMargin(geometryFor(cfg, /*fractionalScale=*/false), edge) == 0);
    TEST_CHECK(edgeMargin(geometryFor(cfg, /*fractionalScale=*/true), edge) == -1);

    // Surface size and reserved space describe the dock itself and must not move with the
    // overlap, or the dock would reserve a pixel it does not paint.
    const auto integerGeometry = geometryFor(cfg, /*fractionalScale=*/false);
    const auto fractionalGeometry = geometryFor(cfg, /*fractionalScale=*/true);
    TEST_CHECK(integerGeometry.surfaceW == fractionalGeometry.surfaceW);
    TEST_CHECK(integerGeometry.surfaceH == fractionalGeometry.surfaceH);
    TEST_CHECK(integerGeometry.exclusiveZone == fractionalGeometry.exclusiveZone);
  }

  // Magnification reserves surface space away from the screen edge. Instance-count badges face
  // inward, so enabling them must not move a flush panel away from its anchored edge.
  for (const DockEdge edge : kEdges) {
    DockConfig cfg = flushDock(edge);
    cfg.magnification = true;
    cfg.magnificationScale = 1.45F;
    cfg.showInstanceCount = true;
    const auto surface = geometryFor(cfg, /*fractionalScale=*/false);
    const ShellConfig::ShadowConfig shadow;
    const auto panel = shell::dock::computePanelGeometry(
        cfg, shadow, static_cast<float>(surface.surfaceW), static_cast<float>(surface.surfaceH)
    );
    TEST_CHECK(panelTouchesSurfaceEdge(panel, surface, edge));
  }

  {
    DockConfig top = flushDock(DockEdge::Top);
    top.magnification = true;
    top.showInstanceCount = true;
    DockConfig bottom = top;
    bottom.position = DockEdge::Bottom;
    TEST_CHECK(
        geometryFor(top, /*fractionalScale=*/false).surfaceH == geometryFor(bottom, /*fractionalScale=*/false).surfaceH
    );
  }

  // A dock held off the edge by a margin is already clear of the rounding, so it never overlaps.
  for (const DockEdge edge : kEdges) {
    DockConfig cfg = flushDock(edge);
    cfg.marginEdge = 8;
    TEST_CHECK(edgeMargin(geometryFor(cfg, /*fractionalScale=*/true), edge) >= 0);
  }

  // The hidden auto-hide trigger strip keeps its on-screen thickness: the part of the surface
  // pushed past the output edge cannot be hovered.
  {
    DockConfig cfg = flushDock(DockEdge::Bottom);
    cfg.autoHide = true;
    const auto integerRegion =
        shell::dock::computeInputRegion(cfg, shell::dock::DockPanelGeometry{}, 200, 60, true, false);
    const auto fractionalRegion =
        shell::dock::computeInputRegion(cfg, shell::dock::DockPanelGeometry{}, 200, 60, true, true);
    TEST_CHECK(integerRegion.size() == 1);
    TEST_CHECK(fractionalRegion.size() == 1);
    TEST_CHECK(fractionalRegion[0].height == integerRegion[0].height + 1);
    TEST_CHECK(fractionalRegion[0].y == integerRegion[0].y - 1);
  }

  // Overflowing docks stop at the output ends so their item viewport can scroll.
  for (const DockEdge edge : kEdges) {
    DockConfig cfg = flushDock(edge);
    cfg.marginEnds = 12;
    const ShellConfig::ShadowConfig shadow;
    const auto geometry = shell::dock::computeSurfaceGeometry(
        cfg, shadow, 100, false, /*outputLogicalWidth=*/800, /*outputLogicalHeight=*/600
    );
    const std::uint32_t expectedLength = shell::dock::isVerticalEdge(edge) ? 576U : 776U;
    const std::uint32_t actualLength = shell::dock::isVerticalEdge(edge) ? geometry.surfaceH : geometry.surfaceW;
    TEST_CHECK(actualLength == expectedLength);
  }

  return 0;
}
