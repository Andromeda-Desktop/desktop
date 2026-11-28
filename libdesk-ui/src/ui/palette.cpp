#include "ui/palette.h"

#include "core/log.h"
#include "render/core/color.h"
#include "render/scene/node.h"
#include "design_tokens.h"
#include "protocols/glass_material.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <mutex>
#include <string>

Palette palette = tokenPalette(false);
bool g_resolvedThemeLight = false;

namespace {

  // Serializes setPalette() writes against colorForRoleSnapshot() reads from the
  // script runtime thread. All other palette reads (colorForRole, rendering) stay
  // on the main thread, so this holds only while setPalette() is main-thread-only.
  std::mutex g_paletteMutex;

  std::string normalizedRoleToken(std::string_view token) {
    std::string normalized = StringUtils::trim(token);
    StringUtils::toLowerInPlace(normalized);
    return normalized;
  }

  constexpr auto kLog = Logger("palette");

  // Material role names from older configs and plugins.
  constexpr std::array<ColorRoleToken, 27> kMaterialRoleAliases = {{
      {ColorRole::Accent, "primary"},
      {ColorRole::OnAccent, "on_primary"},
      {ColorRole::Fill1, "primary_container"},
      {ColorRole::Label1, "on_primary_container"},
      {ColorRole::Accent, "secondary"},
      {ColorRole::OnAccent, "on_secondary"},
      {ColorRole::Fill1, "secondary_container"},
      {ColorRole::Label1, "on_secondary_container"},
      {ColorRole::Accent, "tertiary"},
      {ColorRole::OnAccent, "on_tertiary"},
      {ColorRole::Fill1, "tertiary_container"},
      {ColorRole::Label1, "on_tertiary_container"},
      {ColorRole::Danger, "error"},
      {ColorRole::OnAccent, "on_error"},
      {ColorRole::Fill1, "error_container"},
      {ColorRole::Label1, "on_error_container"},
      {ColorRole::SolidRegular, "surface"},
      {ColorRole::Label1, "on_surface"},
      {ColorRole::Label2, "on_surface_variant"},
      {ColorRole::SolidThin, "surface_container_low"},
      {ColorRole::Fill2, "surface_container"},
      {ColorRole::Fill3, "surface_container_high"},
      {ColorRole::Fill3, "surface_container_highest"},
      {ColorRole::Stroke, "outline_variant"},
      {ColorRole::Shadow, "scrim"},
      {ColorRole::Fill3, "hover"},
      {ColorRole::Label1, "on_hover"},
  }};

  Color tokenColor(std::uint32_t rgba) noexcept {
    return Color{static_cast<float>(rgba >> 24) / 255.0F, static_cast<float>((rgba >> 16) & 0xffU) / 255.0F,
                 static_cast<float>((rgba >> 8) & 0xffU) / 255.0F, static_cast<float>(rgba & 0xffU) / 255.0F};
  }

  Palette g_darkPalette = tokenPalette(false);
  Palette g_lightPalette = tokenPalette(true);

  const Color& roleIn(const Palette& p, ColorRole role) noexcept {
    switch (role) {
    case ColorRole::Label1:
      return p.label1;
    case ColorRole::Label2:
      return p.label2;
    case ColorRole::Fill1:
      return p.fill1;
    case ColorRole::Fill2:
      return p.fill2;
    case ColorRole::Fill3:
      return p.fill3;
    case ColorRole::SolidThin:
      return p.solidThin;
    case ColorRole::SolidRegular:
      return p.solidRegular;
    case ColorRole::Stroke:
      return p.stroke;
    case ColorRole::Accent:
      return p.accent;
    case ColorRole::OnAccent:
      return p.onAccent;
    case ColorRole::Danger:
      return p.danger;
    case ColorRole::Shadow:
      return p.shadow;
    case ColorRole::TintRegular:
      return p.tintRegular;
    case ColorRole::TintThick:
      return p.tintThick;
    case ColorRole::Label3:
      return p.label3;
    case ColorRole::AccentText:
      return p.accentText;
    case ColorRole::DangerText:
      return p.dangerText;
    case ColorRole::Separator:
      return p.separator;
    case ColorRole::Lens:
      return p.lens;
    case ColorRole::LensEdge:
      return p.lensEdge;
    case ColorRole::Warning:
      return p.warning;
    case ColorRole::Success:
      return p.success;
    case ColorRole::SliderFill:
      return p.sliderFill;
    case ColorRole::OnSliderFill:
      return p.onSliderFill;
    case ColorRole::SliderTrack:
      return p.sliderTrack;
    case ColorRole::SliderEdge:
      return p.sliderEdge;
    case ColorRole::GlassCard:
      return p.glassCard;
    case ColorRole::GlassCardEdge:
      return p.glassCardEdge;
    case ColorRole::Popover:
      return p.popover;
    case ColorRole::View:
      return p.view;
    case ColorRole::TermRed:
      return p.termRed;
    case ColorRole::TermGreen:
      return p.termGreen;
    case ColorRole::TermYellow:
      return p.termYellow;
    case ColorRole::TermBlue:
      return p.termBlue;
    case ColorRole::TermMagenta:
      return p.termMagenta;
    case ColorRole::TermCyan:
      return p.termCyan;
    case ColorRole::TermFindHit:
      return p.termFindHit;
    case ColorRole::TermFindHitCurrent:
      return p.termFindHitCurrent;
    case ColorRole::TermFindRing:
      return p.termFindRing;
    case ColorRole::TermCommandBand:
      return p.termCommandBand;
    case ColorRole::Card:
      return p.card;
    case ColorRole::DiffAdd:
      return p.diffAdd;
    case ColorRole::DiffDel:
      return p.diffDel;
    case ColorRole::Scrim:
      return p.scrim;
    }

    return p.label1;
  }

} // namespace

const Color& colorForRole(ColorRole role) noexcept { return roleIn(palette, role); }

Color colorForRole(ColorRole role, float alpha) noexcept {
  Color color = colorForRole(role);
  color.a *= alpha;
  return color;
}

const Palette& paletteForTone(PaletteTone tone) noexcept {
  switch (tone) {
  case PaletteTone::Scheme:
    return palette;
  case PaletteTone::Dark:
    return g_darkPalette;
  case PaletteTone::Light:
    return g_lightPalette;
  }
  return palette;
}

void setDarkPalette(const Palette& p) { g_darkPalette = p; }

void setLightPalette(const Palette& p) { g_lightPalette = p; }

PaletteTone toneOn(const Color& surface) noexcept {
  return relativeLuminance(surface) > 0.179F ? PaletteTone::Light : PaletteTone::Dark;
}

const Color& colorForRole(ColorRole role, const Palette& p) noexcept { return roleIn(p, role); }

const Color& colorForRole(ColorRole role, PaletteTone tone) noexcept { return roleIn(paletteForTone(tone), role); }

PaletteTone glassTone(glass::Kind kind) noexcept {
  return glass::tone(kind, Style::glassStyle()) == glass::Tone::Dark ? PaletteTone::Dark : PaletteTone::Scheme;
}

const Color& colorForRole(ColorRole role, const Node* node) noexcept {
  return roleIn(node != nullptr ? paletteForTone(node->paletteTone()) : palette, role);
}

Color colorForRole(ColorRole role, float alpha, const Node* node) noexcept {
  Color color = colorForRole(role, node);
  color.a *= alpha;
  return color;
}

std::optional<ColorRole> colorRoleFromToken(std::string_view token) {
  const std::string normalized = normalizedRoleToken(token);
  for (const auto& option : kColorRoleTokens) {
    if (normalized == option.token) {
      return option.role;
    }
  }
  for (const auto& option : kMaterialRoleAliases) {
    if (normalized == option.token) {
      return option.role;
    }
  }
  return std::nullopt;
}

std::optional<ColorRole> colorRoleFromPluginToken(std::string_view token) {
  if (auto role = colorRoleFromToken(token)) {
    return role;
  }
  const std::string normalized = normalizedRoleToken(token);
  if (normalized == "surface_variant") {
    static std::once_flag warned;
    std::call_once(warned, [] {
      kLog.warn(R"(plugin color role "surface_variant" is deprecated; use "fill_2")");
    });
    return ColorRole::Fill2;
  }
  if (normalized == "outline") {
    static std::once_flag warned;
    std::call_once(warned, [] { kLog.warn(R"(plugin color role "outline" is deprecated; use "stroke")"); });
    return ColorRole::Stroke;
  }
  return std::nullopt;
}

std::string_view colorRoleToken(ColorRole role) noexcept {
  for (const auto& option : kColorRoleTokens) {
    if (option.role == role) {
      return option.token;
    }
  }
  return "label_1";
}

ColorSpec colorSpecFromRole(ColorRole role, float alpha) noexcept {
  return ColorSpec{.role = role, .fixed = clearColor(), .alpha = alpha};
}

ColorSpec fixedColorSpec(const Color& color) noexcept {
  return ColorSpec{.role = std::nullopt, .fixed = color, .alpha = 1.0F};
}

Color resolveColorSpec(const ColorSpec& color) noexcept {
  Color resolved = color.role.has_value() ? colorForRole(*color.role) : color.fixed;
  resolved.a *= color.alpha;
  return resolved;
}

Color resolveColorSpec(const ColorSpec& color, const Node* node) noexcept {
  Color resolved = color.role.has_value() ? colorForRole(*color.role, node) : color.fixed;
  if (color.role.has_value() && node != nullptr) {
    if (const auto fade = node->paletteToneFade(); fade.has_value()) {
      resolved = lerpColor(roleIn(paletteForTone(fade->from), *color.role), resolved, fade->progress);
    }
    const ColorRole role = *color.role;
    if (const auto lift = node->inkLift();
        lift.has_value() && (role == ColorRole::Label1 || role == ColorRole::Label2 || role == ColorRole::Label3)) {
      if (role == ColorRole::Label1) {
        resolved.a = std::max(resolved.a, glass::kGlassInk1);
      } else {
        if (role == ColorRole::Label2) {
          resolved.a = std::max(resolved.a, glass::kGlassInk2);
        }
        // Labels are black or white at an opacity: light ink sits on dark glass.
        const int tone = resolved.r + resolved.g + resolved.b > 1.5F ? 1 : 0;
        const float legible = role == ColorRole::Label2 ? glass::kLegibleInk2[tone] : glass::kLegibleInk3[tone];
        resolved.a = std::max(resolved.a, std::lerp(resolved.a, legible, *lift));
      }
    }
  }
  resolved.a *= color.alpha;
  return resolved;
}

bool isLightPalette() noexcept { return relativeLuminance(palette.solidRegular) > 0.179F; }

bool isResolvedLightTheme() noexcept { return g_resolvedThemeLight; }

void setResolvedThemeLight(bool light) noexcept { g_resolvedThemeLight = light; }

Signal<>& paletteChanged() {
  static Signal<> signal;
  return signal;
}

void setPalette(const Palette& p) {
  {
    std::scoped_lock lock(g_paletteMutex);
    if (palette == p) {
      return;
    }
    palette = p;
  }
  paletteChanged().emit();
}

Color colorForRoleSnapshot(ColorRole role) {
  std::scoped_lock lock(g_paletteMutex);
  return colorForRole(role);
}

Palette lerpPalette(const Palette& a, const Palette& b, float t) {
  return Palette{
      .label1 = lerpColor(a.label1, b.label1, t),
      .label2 = lerpColor(a.label2, b.label2, t),
      .fill1 = lerpColor(a.fill1, b.fill1, t),
      .fill2 = lerpColor(a.fill2, b.fill2, t),
      .fill3 = lerpColor(a.fill3, b.fill3, t),
      .solidThin = lerpColor(a.solidThin, b.solidThin, t),
      .solidRegular = lerpColor(a.solidRegular, b.solidRegular, t),
      .stroke = lerpColor(a.stroke, b.stroke, t),
      .accent = lerpColor(a.accent, b.accent, t),
      .onAccent = lerpColor(a.onAccent, b.onAccent, t),
      .danger = lerpColor(a.danger, b.danger, t),
      .shadow = lerpColor(a.shadow, b.shadow, t),
      .tintRegular = lerpColor(a.tintRegular, b.tintRegular, t),
      .tintThick = lerpColor(a.tintThick, b.tintThick, t),
      .label3 = lerpColor(a.label3, b.label3, t),
      .accentText = lerpColor(a.accentText, b.accentText, t),
      .dangerText = lerpColor(a.dangerText, b.dangerText, t),
      .separator = lerpColor(a.separator, b.separator, t),
      .lens = lerpColor(a.lens, b.lens, t),
      .lensEdge = lerpColor(a.lensEdge, b.lensEdge, t),
      .warning = lerpColor(a.warning, b.warning, t),
      .success = lerpColor(a.success, b.success, t),
      .sliderFill = lerpColor(a.sliderFill, b.sliderFill, t),
      .onSliderFill = lerpColor(a.onSliderFill, b.onSliderFill, t),
      .sliderTrack = lerpColor(a.sliderTrack, b.sliderTrack, t),
      .sliderEdge = lerpColor(a.sliderEdge, b.sliderEdge, t),
      .glassCard = lerpColor(a.glassCard, b.glassCard, t),
      .glassCardEdge = lerpColor(a.glassCardEdge, b.glassCardEdge, t),
      .popover = lerpColor(a.popover, b.popover, t),
      .view = lerpColor(a.view, b.view, t),
      .termRed = lerpColor(a.termRed, b.termRed, t),
      .termGreen = lerpColor(a.termGreen, b.termGreen, t),
      .termYellow = lerpColor(a.termYellow, b.termYellow, t),
      .termBlue = lerpColor(a.termBlue, b.termBlue, t),
      .termMagenta = lerpColor(a.termMagenta, b.termMagenta, t),
      .termCyan = lerpColor(a.termCyan, b.termCyan, t),
      .termFindHit = lerpColor(a.termFindHit, b.termFindHit, t),
      .termFindHitCurrent = lerpColor(a.termFindHitCurrent, b.termFindHitCurrent, t),
      .termFindRing = lerpColor(a.termFindRing, b.termFindRing, t),
      .termCommandBand = lerpColor(a.termCommandBand, b.termCommandBand, t),
      .card = lerpColor(a.card, b.card, t),
      .diffAdd = lerpColor(a.diffAdd, b.diffAdd, t),
      .diffDel = lerpColor(a.diffDel, b.diffDel, t),
      .scrim = lerpColor(a.scrim, b.scrim, t),
  };
}

void applyHighContrast(Palette& p) noexcept {
  p.label2 = p.label1;
  p.stroke.a = 1.0F;
}

Palette tokenPalette(bool light) noexcept {
  const auto& s = light ? Tokens::schemeLight : Tokens::schemeDark;
  return Palette{
      .label1 = tokenColor(s.label1),
      .label2 = tokenColor(s.label2),
      .fill1 = tokenColor(s.fill1),
      .fill2 = tokenColor(s.fill2),
      .fill3 = tokenColor(s.fill3),
      .solidThin = tokenColor(s.solidThin),
      .solidRegular = tokenColor(s.solidRegular),
      .stroke = tokenColor(s.stroke),
      .accent = tokenColor(s.accent),
      .onAccent = tokenColor(s.onAccent),
      .danger = tokenColor(s.danger),
      .shadow = tokenColor(s.shadow),
      .tintRegular = tokenColor(s.tintRegular | 0xffU), // callers carry the alpha (Style::glassTint)
      .tintThick = tokenColor(s.tintThick | 0xffU),
      .label3 = tokenColor(s.label3),
      .accentText = tokenColor(s.accentText),
      .dangerText = tokenColor(s.dangerText),
      .separator = tokenColor(s.separator),
      .lens = tokenColor(s.lens),
      .lensEdge = tokenColor(s.lensEdge),
      .warning = tokenColor(s.warning),
      .success = tokenColor(s.success),
      .sliderFill = tokenColor(s.sliderFill),
      .onSliderFill = tokenColor(s.onSliderFill),
      .sliderTrack = tokenColor(s.sliderTrack),
      .sliderEdge = tokenColor(s.sliderEdge),
      .glassCard = tokenColor(s.glassCard),
      .glassCardEdge = tokenColor(s.glassCardEdge),
      .popover = tokenColor(s.popover),
      .view = tokenColor(s.view),
      .termRed = tokenColor(s.termRed),
      .termGreen = tokenColor(s.termGreen),
      .termYellow = tokenColor(s.termYellow),
      .termBlue = tokenColor(s.termBlue),
      .termMagenta = tokenColor(s.termMagenta),
      .termCyan = tokenColor(s.termCyan),
      .termFindHit = tokenColor(s.termFindHit),
      .termFindHitCurrent = tokenColor(s.termFindHitCurrent),
      .termFindRing = tokenColor(s.termFindRing),
      .termCommandBand = tokenColor(s.termCommandBand),
      .card = tokenColor(s.card),
      .diffAdd = tokenColor(s.diffAdd),
      .diffDel = tokenColor(s.diffDel),
      .scrim = tokenColor(s.scrim),
  };
}

std::string iconColorSchemeStylesheet(const Palette& colors) {
  const std::array<std::pair<std::string_view, const Color*>, 8> classes{{
      {"Text", &colors.label1},
      {"Background", &colors.view},
      {"Highlight", &colors.accent},
      {"HighlightedText", &colors.onAccent},
      {"PositiveText", &colors.success},
      {"NeutralText", &colors.warning},
      {"NegativeText", &colors.danger},
      {"Accent", &colors.accent},
  }};
  std::string stylesheet;
  for (const auto& [name, color] : classes) {
    stylesheet += std::format(".ColorScheme-{} {{ color: {} !important; }}\n", name, formatRgbHex(*color));
  }
  return stylesheet;
}
