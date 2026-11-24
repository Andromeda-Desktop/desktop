#pragma once

#include "render/core/color.h"

#include <array>
#include <optional>

// The colours a theme author picks; everything else is derived from them.
struct ThemeAnchors {
  Color surface;
  Color text;
  Color accent;
  std::optional<std::array<Color, 16>> ansi; // the author's own ANSI colours; replaces the derived ones
};

// Text colours reach their ratio against view, or the best ratio black or white can reach there.
struct ThemePalette {
  Color view;   // the surface anchor, never changed
  Color label1; // >= 7:1
  Color label2; // >= 4.5:1 and dimmer than label1
  Color accent; // >= 3:1
  Color selection; // view mixed 40 % toward accent
  // Derived: 0 is a raised surface, 8 is label2, every other entry >= 7:1; 1-6 share one OKLCH lightness, their hues
  // turned up to 15 degrees toward the accent. Author's: each entry but 0 lifted to >= 4.5:1 by lightness only.
  std::array<Color, 16> ansi;
};

[[nodiscard]] ThemePalette deriveThemePalette(const ThemeAnchors& anchors);
