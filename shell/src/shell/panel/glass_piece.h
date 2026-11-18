#pragma once

#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/style.h"

// Makes the node one glass piece of a panel that usesGlassPieces(): the compositor draws its glass; without a
// compositor tint the node paints the card fill itself.
inline void styleGlassPiece(Flex& node, float radius, float scale, float opacity) {
  if (Style::compositorDrawsTint()) {
    node.setFill(clearColorSpec());
  } else {
    node.setFill(colorSpecFromRole(ColorRole::GlassCard, opacity));
    node.setBorder(colorSpecFromRole(ColorRole::GlassCardEdge), 0.5F * scale);
  }
  node.setRadius(radius);
  node.setGlassPiece(radius);
}
