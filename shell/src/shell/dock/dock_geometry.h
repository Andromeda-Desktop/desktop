#pragma once

#include "config/config_types.h"
#include "render/core/render_styles.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

struct InputRect;
struct LayerSurfaceConfig;

namespace shell::dock {

  struct DockPanelGeometry {
    float panelX = 0.0F;
    float panelY = 0.0F;
    float panelW = 0.0F;
    float panelH = 0.0F;
  };

  struct DockSurfaceGeometry {
    std::uint32_t surfaceW = 0;
    std::uint32_t surfaceH = 0;
    std::int32_t marginTop = 0;
    std::int32_t marginRight = 0;
    std::int32_t marginBottom = 0;
    std::int32_t marginLeft = 0;
    std::int32_t exclusiveZone = 0;
  };

  struct DockConcaveShape {
    CornerShapes corners{};
    Radii radii;
    RectInsets logicalInset{};
  };

  [[nodiscard]] DockConcaveShape dockConcaveShape(const DockConfig& cfg);

  [[nodiscard]] std::uint32_t positionToAnchor(DockEdge edge);
  [[nodiscard]] bool isVerticalEdge(DockEdge edge);
  void shiftAlongEdge(DockEdge edge, float& x, float& y, float amount);
  // The built-in component.dock.icon: the icon size the dock's glyph, lens and badge tokens are drawn at. A theme's or
  // the user's dock.icon writes the live token, so ratios divide by this, never by the live token.
  [[nodiscard]] float designIconSize();
  // Main-axis cell of a launcher or trash glyph: the glyph's own size, so its gaps equal the gaps between app icons.
  [[nodiscard]] std::int32_t dockGlyphCellSize(const DockConfig& cfg);
  // itemCount counts every cell; glyphCells of them are dockGlyphCellSize long instead of an icon cell.
  [[nodiscard]] std::int32_t dockContentSize(
      const DockConfig& cfg, std::size_t itemCount, std::size_t separators = 0, std::size_t glyphCells = 0
  );
  [[nodiscard]] std::int32_t dockThickness(const DockConfig& cfg);
  // Extra cross-axis surface padding so magnified icons (and badges) are not clipped.
  [[nodiscard]] std::int32_t dockHoverZoomCrossPad(const DockConfig& cfg);
  // Extra main-axis end padding for magnified icon/badge overhang past the pill ends.
  [[nodiscard]] std::int32_t dockHoverZoomMainPad(const DockConfig& cfg);
  // The largest icon size from restIconSize down to component.dock.icon_min whose row and zoom room fit outputLength
  // inside marginEdge per end; past that floor the row scrolls.
  [[nodiscard]] std::int32_t dockFitIconSize(
      const DockConfig& cfg, std::int32_t restIconSize, std::size_t itemCount, std::size_t separators,
      std::size_t glyphCells, std::int32_t outputLength
  );
  // Main-axis offsets of icons drawn at `scales` (1 = rest) so no two grown icons overlap: each moves by the growth of
  // the icons before it plus half its own; the largest stays put and the rest spread out on both sides. sizes[i] is
  // icon i's rest main-axis size.
  [[nodiscard]] std::vector<float> hoverSpreadOffsets(const std::vector<float>& scales, const std::vector<float>& sizes);
  [[nodiscard]] std::size_t dockLauncherButtonCount(DockLauncherPosition position);
  [[nodiscard]] std::size_t dockLauncherButtonCount(const DockConfig& cfg);
  // Cells drawn as a glyph (launcher without a custom image, trash), which use dockGlyphCellSize.
  [[nodiscard]] std::size_t dockGlyphCells(const DockConfig& cfg, std::size_t trashCells);
  // `fractionalScale` marks an output whose scale is not an integer: a flush dock overlaps the
  // screen edge by a logical pixel there, because the compositor can otherwise round the edge a
  // device pixel short and leave a gap.
  [[nodiscard]] DockSurfaceGeometry computeSurfaceGeometry(
      const DockConfig& cfg, const ShellConfig::ShadowConfig& shadow, std::size_t itemCount, bool fractionalScale,
      std::int32_t outputLogicalWidth, std::int32_t outputLogicalHeight, std::size_t separators = 0,
      std::size_t glyphCells = 0
  );
  [[nodiscard]] LayerSurfaceConfig makeLayerSurfaceConfig(
      const DockConfig& cfg, const ShellConfig::ShadowConfig& shadow, std::size_t itemCount, bool fractionalScale,
      std::int32_t outputLogicalWidth, std::int32_t outputLogicalHeight,
      std::size_t separators = 0, std::size_t glyphCells = 0
  );
  [[nodiscard]] DockPanelGeometry
  computePanelGeometry(const DockConfig& cfg, const ShellConfig::ShadowConfig& shadow, float surfaceW, float surfaceH);
  [[nodiscard]] std::pair<float, float> computeHiddenSlideDelta(
      const DockConfig& cfg, const ShellConfig::ShadowConfig& shadow, float surfaceW, float surfaceH,
      const DockPanelGeometry& panel
  );
  [[nodiscard]] std::vector<InputRect> computeInputRegion(
      const DockConfig& cfg, const DockPanelGeometry& panel, int surfaceW, int surfaceH, bool hidden,
      bool fractionalScale
  );

} // namespace shell::dock
