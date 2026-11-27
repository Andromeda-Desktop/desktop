#pragma once

#include "render/core/color.h"
#include "ui/signal.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

class Node;
enum class PaletteTone : std::uint8_t;

// Shell color roles: the [scheme.*] roles of protocol/design-tokens.toml that the shell draws with.
enum class ColorRole : std::uint8_t {
  Label1,
  Label2,
  Fill1,
  Fill2,
  Fill3,
  SolidThin,
  SolidRegular,
  Stroke,
  Accent,
  OnAccent,
  Danger,
  Shadow,
  TintRegular,
  TintThick,
  Label3,
  AccentText,
  DangerText,
  Separator,
  Lens,
  LensEdge,
  Warning,
  Success,
  SliderFill,
  OnSliderFill,
  SliderTrack,
  SliderEdge,
  GlassCard,
  GlassCardEdge,
  Popover,
  View,
  TermRed,
  TermGreen,
  TermYellow,
  TermBlue,
  TermMagenta,
  TermCyan,
  TermFindHit,
  TermFindHitCurrent,
  TermFindRing,
  TermCommandBand,
  Card,
  DiffAdd,
  DiffDel,
  // Internal: the modal dim layer. No token entry, so the legacy name "scrim" still maps to Shadow.
  Scrim,
};

struct ColorRoleToken {
  ColorRole role;
  std::string_view token;
};

inline constexpr std::array<ColorRoleToken, 43> kColorRoleTokens = {{
    {ColorRole::Label1, "label_1"},
    {ColorRole::Label2, "label_2"},
    {ColorRole::Fill1, "fill_1"},
    {ColorRole::Fill2, "fill_2"},
    {ColorRole::Fill3, "fill_3"},
    {ColorRole::SolidThin, "solid_thin"},
    {ColorRole::SolidRegular, "solid_regular"},
    {ColorRole::Stroke, "stroke"},
    {ColorRole::Accent, "accent"},
    {ColorRole::OnAccent, "on_accent"},
    {ColorRole::Danger, "danger"},
    {ColorRole::Shadow, "shadow"},
    {ColorRole::TintRegular, "tint_regular"},
    {ColorRole::TintThick, "tint_thick"},
    {ColorRole::Label3, "label_3"},
    {ColorRole::AccentText, "accent_text"},
    {ColorRole::DangerText, "danger_text"},
    {ColorRole::Separator, "separator"},
    {ColorRole::Lens, "lens"},
    {ColorRole::LensEdge, "lens_edge"},
    {ColorRole::Warning, "warning"},
    {ColorRole::Success, "success"},
    {ColorRole::SliderFill, "slider_fill"},
    {ColorRole::OnSliderFill, "on_slider_fill"},
    {ColorRole::SliderTrack, "slider_track"},
    {ColorRole::SliderEdge, "slider_edge"},
    {ColorRole::GlassCard, "glass_card"},
    {ColorRole::GlassCardEdge, "glass_card_edge"},
    {ColorRole::Popover, "popover"},
    {ColorRole::View, "view"},
    {ColorRole::TermRed, "term_red"},
    {ColorRole::TermGreen, "term_green"},
    {ColorRole::TermYellow, "term_yellow"},
    {ColorRole::TermBlue, "term_blue"},
    {ColorRole::TermMagenta, "term_magenta"},
    {ColorRole::TermCyan, "term_cyan"},
    {ColorRole::TermFindHit, "term_find_hit"},
    {ColorRole::TermFindHitCurrent, "term_find_hit_current"},
    {ColorRole::TermFindRing, "term_find_ring"},
    {ColorRole::TermCommandBand, "term_command_band"},
    {ColorRole::Card, "card"},
    {ColorRole::DiffAdd, "diff_add"},
    {ColorRole::DiffDel, "diff_del"},
}};

[[nodiscard]] constexpr Color clearColor() noexcept { return rgba(0.0F, 0.0F, 0.0F, 0.0F); }

struct ColorSpec {
  std::optional<ColorRole> role;
  Color fixed = clearColor();
  float alpha = 1.0F;
};

constexpr bool operator==(const ColorSpec& a, const ColorSpec& b) noexcept {
  return a.role == b.role && a.fixed == b.fixed && a.alpha == b.alpha;
}

[[nodiscard]] constexpr ColorSpec clearColorSpec() noexcept {
  return ColorSpec{.role = std::nullopt, .fixed = clearColor(), .alpha = 1.0F};
}

// Multiplies a spec's alpha, keeping any color role intact so theme switches still resolve.
// Distinct from withAlpha(Color, float) in render/core/color.h, which replaces the alpha of an
// already-resolved color and therefore cannot preserve the role.
[[nodiscard]] constexpr ColorSpec scaleAlpha(const ColorSpec& color, float factor) noexcept {
  ColorSpec out = color;
  out.alpha = std::clamp(out.alpha * std::clamp(factor, 0.0F, 1.0F), 0.0F, 1.0F);
  return out;
}

// Runtime UI palette: one color per ColorRole, filled from the design-token scheme.
struct Palette {
  Color label1;
  Color label2;
  Color fill1;
  Color fill2;
  Color fill3;
  Color solidThin;
  Color solidRegular;
  Color stroke;
  Color accent;
  Color onAccent;
  Color danger;
  Color shadow;
  Color tintRegular;
  Color tintThick;
  Color label3;
  Color accentText;
  Color dangerText;
  Color separator;
  Color lens;
  Color lensEdge;
  Color warning;
  Color success;
  Color sliderFill;
  Color onSliderFill;
  Color sliderTrack;
  Color sliderEdge;
  Color glassCard;
  Color glassCardEdge;
  Color popover;
  Color view;
  Color termRed;
  Color termGreen;
  Color termYellow;
  Color termBlue;
  Color termMagenta;
  Color termCyan;
  Color termFindHit;
  Color termFindHitCurrent;
  Color termFindRing;
  Color termCommandBand;
  Color card;
  Color diffAdd;
  Color diffDel;
  Color scrim;

  bool operator==(const Palette&) const = default;
};

extern Palette palette;

[[nodiscard]] const Color& colorForRole(ColorRole role) noexcept;
[[nodiscard]] Color colorForRole(ColorRole role, float alpha) noexcept;
[[nodiscard]] std::optional<ColorRole> colorRoleFromToken(std::string_view token);
// Plugin-facing lookup. Also accepts the retired `surface_variant` and `outline` roles, warning once per name.
[[nodiscard]] std::optional<ColorRole> colorRoleFromPluginToken(std::string_view token);
[[nodiscard]] std::string_view colorRoleToken(ColorRole role) noexcept;
// The role in the palette of `node`'s tone (Node::paletteTone); a null node uses the scheme palette.
[[nodiscard]] const Color& colorForRole(ColorRole role, const Node* node) noexcept;
[[nodiscard]] Color colorForRole(ColorRole role, float alpha, const Node* node) noexcept;
[[nodiscard]] const Palette& paletteForTone(PaletteTone tone) noexcept;
[[nodiscard]] const Color& colorForRole(ColorRole role, PaletteTone tone) noexcept;
// The role in `p`, for a surface that shows a scheme other than the active one.
[[nodiscard]] const Color& colorForRole(ColorRole role, const Palette& p) noexcept;
// The tone of `kind` at the current glass slider (glass::tone).
[[nodiscard]] PaletteTone glassTone(glass::Kind kind) noexcept;
// True when content on shell glass draws a soft shadow at the current glass settings (glass::contentShadow).
[[nodiscard]] bool glassContentShadow() noexcept;
// The shadow content with `ink` draws on shell glass (glass::kContentShadowAlpha).
[[nodiscard]] Color contentShadowColor(const Color& ink) noexcept;
// The palettes of PaletteTone::Dark and ::Light. Set them before setPalette(), which re-resolves every control.
void setDarkPalette(const Palette& p);
void setLightPalette(const Palette& p);
// The tone whose ink reads on `surface` (the 0.179 luminance split of isLightPalette).
[[nodiscard]] PaletteTone toneOn(const Color& surface) noexcept;
[[nodiscard]] ColorSpec colorSpecFromRole(ColorRole role, float alpha = 1.0F) noexcept;
// Glass material fill: the tint over the compositor blur, or the opaque solid when transparency is reduced.
// The palette color is opaque, so a caller's opacity (Style::glassTint) replaces its alpha, never multiplies it.
[[nodiscard]] inline ColorRole materialRole() noexcept {
  return Style::reduceTransparency() ? ColorRole::SolidRegular : ColorRole::TintRegular;
}
// Panel and sheet material: tint_thick.
[[nodiscard]] inline ColorRole panelMaterialRole() noexcept {
  return Style::reduceTransparency() ? ColorRole::SolidRegular : ColorRole::TintThick;
}
[[nodiscard]] ColorSpec fixedColorSpec(const Color& color) noexcept;
[[nodiscard]] Color resolveColorSpec(const ColorSpec& color) noexcept;
[[nodiscard]] Color resolveColorSpec(const ColorSpec& color, const Node* node) noexcept;
[[nodiscard]] bool isLightPalette() noexcept;
// Set by ThemeService from resolved light/dark mode (not inferred from palette colors).
[[nodiscard]] bool isResolvedLightTheme() noexcept;
void setResolvedThemeLight(bool light) noexcept;

void setPalette(const Palette& p);

// CSS for KDE color-scheme icons: each .ColorScheme-* class takes its palette role.
[[nodiscard]] std::string iconColorSchemeStylesheet(const Palette& colors);

[[nodiscard]] Palette tokenPalette(bool light) noexcept;

// Snapshot accessor safe to call from non-main threads (e.g. plugin script workers).
[[nodiscard]] Color colorForRoleSnapshot(ColorRole role);

[[nodiscard]] inline ColorSpec scrollbarTrackColor() noexcept {
  return colorSpecFromRole(ColorRole::Stroke, Style::disabledOutlineAlpha);
}
[[nodiscard]] inline ColorSpec scrollbarThumbColor() noexcept {
  return colorSpecFromRole(ColorRole::Label2, 0.5F);
}
[[nodiscard]] inline ColorSpec focusRingColorSpec() noexcept { return colorSpecFromRole(ColorRole::Accent); }

// Fired after setPalette() writes. Controls subscribe in their constructor
// and re-apply palette-derived colors to their scene nodes on each emit.
Signal<>& paletteChanged();

// Linearly interpolates each field of two palettes in sRGB space. Used by
// ThemeService to drive smooth cross-fade transitions on theme changes.
Palette lerpPalette(const Palette& a, const Palette& b, float t);
