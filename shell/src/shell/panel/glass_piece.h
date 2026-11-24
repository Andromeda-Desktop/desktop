#pragma once

#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/style.h"

// A glass piece's own fill when the compositor draws no tint: the island glass, opaque under Reduce Transparency.
inline ColorSpec glassPieceFill(float opacity) {
  return colorSpecFromRole(materialRole(), Style::islandGlass().tint * opacity);
}

// Makes the node one glass piece of a panel that usesGlassPieces(): the compositor draws its glass; without a
// compositor tint the node paints the glass fill itself.
inline void styleGlassPiece(Flex& node, float radius, float scale, float opacity) {
  if (Style::compositorDrawsTint()) {
    node.setFill(clearColorSpec());
  } else {
    node.setFill(glassPieceFill(opacity));
    node.setBorder(colorSpecFromRole(ColorRole::GlassCardEdge), 0.5F * scale);
  }
  node.setRadius(radius);
  node.setGlassPiece(radius);
}
