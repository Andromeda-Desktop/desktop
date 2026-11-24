#pragma once

#include "ui/controls/button.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>

namespace panel_button_style {

  inline void configureHeaderIconButton(Button& button, float scale) {
    button.setVariant(ButtonVariant::Default);
    // Icon-only header buttons (back, close, header actions) are 32 circles (rulebook 9.5), as in macOS and iOS sheets.
    button.setGlyphSize(Tokens::sizeSymbol * scale);
    button.setMinWidth(Tokens::sizeControlM * scale);
    button.setMinHeight(Tokens::sizeControlM * scale);
    button.setPadding(Tokens::spaceP8 * scale);
    button.setRadius(Tokens::sizeControlM * 0.5F * scale);
  }

  // A 40 round icon button on a glass sheet (CC detail back): the island glass tint over the sheet, with the glass rim.
  inline void configureGlassIconButton(Button& button, float scale) {
    const float tint = Style::islandGlass().tint;
    const ColorSpec rim = colorSpecFromRole(ColorRole::LensEdge);
    const ColorSpec ink = colorSpecFromRole(ColorRole::Label1);
    button.setCustomPalette({
        .borderWidth = scale,
        .normal = {colorSpecFromRole(materialRole(), tint), rim, ink},
        .hover = {colorSpecFromRole(materialRole(), std::min(1.0F, tint + 0.12F)), rim, ink},
        .pressed = {colorSpecFromRole(materialRole(), std::min(1.0F, tint + 0.2F)), rim, ink},
        .disabled = {colorSpecFromRole(materialRole(), tint), rim, colorSpecFromRole(ColorRole::Label3)},
    });
    button.setGlyphSize(Tokens::sizeSymbolL * scale);
    button.setMinWidth(Tokens::sizeControlL * scale);
    button.setMinHeight(Tokens::sizeControlL * scale);
    button.setPadding(0.0F);
    button.setRadius(Tokens::sizeControlL * 0.5F * scale);
  }

} // namespace panel_button_style
