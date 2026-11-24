#include "ui/theme_palette.h"

#include "ui/contrast.h"

#include <algorithm>
#include <cmath>
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
  constexpr double kMaxHueTurn = 15.0;   // degrees toward the accent
  constexpr double kMinAccentChroma = 0.03; // below this the accent hue is noise

  // Turns the hue toward the accent by half the angle between them, at most kMaxHueTurn.
  Hue harmonized(Hue hue, const Oklch& accent) {
    if (accent.chroma < kMinAccentChroma) {
      return hue;
    }
    const double diff = std::remainder(accent.hueRadians * 180.0 / std::numbers::pi - hue.degrees, 360.0);
    hue.degrees += std::copysign(std::min(std::abs(diff) * 0.5, kMaxHueTurn), diff);
    return hue;
  }

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
  p.selection = mixOpaque(p.view, p.accent, 0.4F);

  if (anchors.ansi) {
    for (std::size_t i = 0; i < p.ansi.size(); ++i) {
      const Color given = withOpaqueAlpha((*anchors.ansi)[i]);
      p.ansi[i] = i == 0 ? given : ensureContrast(given, p.view, std::min(4.5, reach), toward);
    }
    return p;
  }

  std::array<Hue, 6> hues = kHues;
  const Oklch accent = toOklch(p.accent);
  std::ranges::transform(hues, hues.begin(), [&](const Hue& h) { return harmonized(h, accent); });
  // The lightness nearest the surface at which all six hues reach the text ratio.
  const double viewLightness = toOklch(p.view).lightness;
  double ring = lighten ? 1.0 : 0.0;
  for (double l = viewLightness; lighten ? l <= 1.0 : l >= 0.0; l += toward * kLightnessStep) {
    if (std::ranges::all_of(hues, [&](const Hue& h) { return contrastRatio(hueAt(h, l), p.view) >= text; })) {
      ring = l;
      break;
    }
  }
  for (std::size_t i = 0; i < hues.size(); ++i) {
    p.ansi[1 + i] = hueAt(hues[i], ring);
    p.ansi[9 + i] = ensureContrast(hueAt(hues[i], ring + toward * kBrightStep), p.view, text, toward);
  }

  Oklch raised = toOklch(p.view);
  raised.lightness = std::clamp(raised.lightness + toward * kBrightStep, 0.0, 1.0);
  p.ansi[0] = fromOklch(raised);
  p.ansi[8] = p.label2;
  p.ansi[7] = ensureContrast(mixOpaque(p.label1, p.view, 0.15F), p.view, text, toward);
  p.ansi[15] = p.label1;
  return p;
}
