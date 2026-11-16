#include "ui/theme_palette.h"

#include "ui/contrast.h"

#include <algorithm>
#include <numbers>

namespace {

  struct Hue {
    double degrees;
    double maxChroma;
  };

  // Red, green, yellow, blue, magenta, cyan; chroma caps keep the screen calm (D-SPEC Theme).
  constexpr std::array<Hue, 6> kHues = {{{27, 0.17}, {145, 0.13}, {85, 0.13}, {262, 0.13}, {325, 0.13}, {205, 0.13}}};
  constexpr double kLightnessStep = 0.005;
  constexpr double kBrightStep = 0.08;

  Color withOpaqueAlpha(Color c) {
    c.a = 1.0F;
    return c;
  }

  Color mixOpaque(const Color& a, const Color& b, float t) {
    return Color{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1.0F};
  }

  Color hueAt(const Hue& hue, double lightness) {
    return fromOklch({.lightness = std::clamp(lightness, 0.0, 1.0),
                      .chroma = hue.maxChroma,
                      .hueRadians = hue.degrees * std::numbers::pi / 180.0});
  }

} // namespace

ThemePalette deriveThemePalette(const ThemeAnchors& anchors) {
  ThemePalette p;
  p.view = withOpaqueAlpha(anchors.surface);
  const Color white{1.0F, 1.0F, 1.0F, 1.0F};
  const Color black{0.0F, 0.0F, 0.0F, 1.0F};
  const bool lighten = contrastRatio(white, p.view) >= contrastRatio(black, p.view);
  const int toward = lighten ? 1 : -1;
  const double reach = contrastRatio(lighten ? white : black, p.view);
  const double text = std::min(7.0, reach);

  p.label1 = ensureContrast(withOpaqueAlpha(anchors.text), p.view, text, toward);
  p.label2 = ensureContrast(mixOpaque(p.label1, p.view, 0.35F), p.view, std::min(4.5, reach), toward);
  p.accent = ensureContrast(withOpaqueAlpha(anchors.accent), p.view, std::min(3.0, reach), toward);

  // The lightness nearest the surface at which all six hues reach the text ratio.
  const double viewLightness = toOklch(p.view).lightness;
  double ring = lighten ? 1.0 : 0.0;
  for (double l = viewLightness; lighten ? l <= 1.0 : l >= 0.0; l += toward * kLightnessStep) {
    if (std::ranges::all_of(kHues, [&](const Hue& h) { return contrastRatio(hueAt(h, l), p.view) >= text; })) {
      ring = l;
      break;
    }
  }
  for (std::size_t i = 0; i < kHues.size(); ++i) {
    p.ansi[1 + i] = hueAt(kHues[i], ring);
    p.ansi[9 + i] = ensureContrast(hueAt(kHues[i], ring + toward * kBrightStep), p.view, text, toward);
  }

  Oklch raised = toOklch(p.view);
  raised.lightness = std::clamp(raised.lightness + toward * kBrightStep, 0.0, 1.0);
  p.ansi[0] = fromOklch(raised);
  p.ansi[8] = p.label2;
  p.ansi[7] = ensureContrast(mixOpaque(p.label1, p.view, 0.15F), p.view, text, toward);
  p.ansi[15] = p.label1;
  return p;
}
