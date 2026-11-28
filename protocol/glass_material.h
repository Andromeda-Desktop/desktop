#pragma once

// The Glass style and slider mapping (CLAUDE.md Decisions "Glass slider", "One glass system"), shared so the shell
// (through libdesk-ui Style::glassMaterial) and the compositor use the same values. No other code maps them. The
// tuning values and the functions that read them live in glass_material.cpp, so a tuning change rebuilds one file.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace glass {

  // Shape: small liquid shapes (bar islands, dock, center island, launcher pill). Sheet: panels, sheets, cards,
  // windows. Menu: menus and popovers. Tooltip: tooltips (denser tint for small text).
  enum class Kind : std::uint8_t { Shape, Sheet, Menu, Tooltip };

  struct Material {
    // Tint alpha over the blur at the kind's reference opacity; 1 = solid (Reduce Transparency).
    float tint = 1.0F;
    // dsk_surface_v1 liquid [0]: rim refraction strength (the Liquid slider).
    float level = 0.0F;
    // dsk_surface_v1 liquid [1]: the specular rim.
    float rim = 0.0F;
    // dsk_surface_v1 liquid [2] (v9): the sharp backdrop through the rim band; costs a backdrop copy.
    float lens = 0.0F;
    // dsk_surface_v1 blur [0] (v15): how much lighter the blur is, 0 full size to 1 the lightest (the Blur slider).
    float clarity = 0.0F;
    // Blur size, 0 to 1: blurStrength(clarity).
    float blurStrength = 1.0F;
    // dsk_surface_v1 liquid [3]: the fraction of the gap to opaque a full density closes (denser, denseLift).
    float dense = 0.0F;

    bool operator==(const Material&) const = default;
  };

  // Ink alpha label_2 and label_3 (dim by design) reach at full density:
  // {light glass (dark ink), dark glass (light ink)}. Secondary ink rises to these with the surface's density.
  extern const float kLegibleInk2[2];
  extern const float kLegibleInk3[2];
  // Least ink alpha label_1 and label_2 take on any glass surface: the tokens' alpha lets the backdrop through.
  extern const float kGlassInk1;
  extern const float kGlassInk2;

  // The palette a kind's glass and content take. Dark: dark glass with white content in both schemes.
  enum class Tone : std::uint8_t { Scheme, Dark };

  // Liquid and Clear glass (refraction > 0) shows the backdrop as a glass body: {saturation gain, dim}, the backdrop
  // saturated by 1 + gain and darkened by 1 - dim before the tint.
  extern const float kVibrancy[2];
  // Glass shapes of one surface with the same material join by a smooth union within this many logical px, so shapes
  // that move together merge and split like one liquid (gaps under about half of it bridge).
  extern const float kMergeReach;
  // Glass rims within this many logical px of the pointer turn their key light toward it.
  extern const float kPointerLightReach;
  // Clarity 0 blurs at full size, 1 not at all.
  [[nodiscard]] constexpr float blurStrength(float clarity) noexcept { return 1.0F - clarity; }

  // Frosted: blur and tint from the Tint and Blur sliders, an optional specular edge. Liquid: a soft blur with a lift, a
  // convex lens bezel at the edge. Clear: Liquid with kClearTint, white content, and a light dim (denseLift) over
  // bright backdrops. Tinted: Frosted at the design tint and blur (kTintedLevel).
  // The values are the portal's glass-style numbers.
  enum class Style : std::uint8_t { Frosted, Liquid, Clear, Tinted };

  // Shape tints at the lowest and the middle Tint slider position: {light glass, dark glass}.
  extern const float kLowestTint[2];
  extern const float kTintedTint[2];
  // Liquid shape tint at the middle Tint slider position (the lowest is kClearTint): the backdrop stays visible, and
  // the density path (adaptDensity) adds the style's light overlay over a backdrop of the content's tone.
  extern const float kLiquidTint[2];
  // The tint alpha of Clear glass: enough to carry a tint colour, so the compositor can dim through the density.
  extern const float kClearTint;

  // Material::dense: the fraction of the gap to opaque every tint closes at full density, the style's light overlay:
  // glass keeps its look over any backdrop, and text may fall below 4.5:1 (docs/design/deviations.md "Glass density").
  // It stays below 1, so the slider order holds. Tooltips close half.
  [[nodiscard]] float denseLift(Kind kind, Style style, bool lightGlass) noexcept;

  // Shell glass follows the scheme; Clear is dark glass with white content in both schemes (docs/design/deviations.md
  // "Clear glass tone"). App windows keep the scheme (Style::glassMaterial with PaletteTone::Scheme).
  [[nodiscard]] constexpr Tone tone(Kind kind, Style style) noexcept {
    (void)kind;
    return style == Style::Clear ? Tone::Dark : Tone::Scheme;
  }

  // The Glass settings (appearance.glass_style and the sliders of each style). Each style keeps its own tint and blur.
  struct Settings {
    Style style = Style::Tinted;
    // Frosted Tint (appearance.glass): 0 Clear, kTintedLevel Tinted, 1 Frosted.
    float tint = 1.0F;
    // Frosted Blur (appearance.frost): 0 the lightest blur, kTintedLevel Tinted, 1 full size.
    float frost = 1.0F;
    // Specular edge of every style and its pointer light (appearance.edge_reflection).
    bool edgeReflection = true;
    // Liquid Tint (appearance.liquid_tint), on the Tint scale.
    float liquidTint = 0.5F;
    // Liquid Blur (appearance.liquid_blur): 0 Soft, 1 Strong (kLiquidClarity).
    float liquidBlur = 0.1F;
    // Liquid Lens (appearance.liquid): 0 Subtle, 0.5 the design values, 1 Strong.
    float lens = 0.5F;
    bool reduceTransparency = false;

    [[nodiscard]] float activeTint() const noexcept;
    [[nodiscard]] float activeFrost() const noexcept;
    bool operator==(const Settings&) const = default;
  };

  // App windows take Frosted when the style is Clear: Clear glass is for shell surfaces only.
  [[nodiscard]] constexpr Settings windowSettings(Settings settings) noexcept {
    if (settings.style == Style::Clear) {
      settings.style = Style::Frosted;
    }
    return settings;
  }

  // Opacity a configured `opacity` of `kind` is relative to: tint_regular (0.70) for shapes, tint_thick otherwise.
  [[nodiscard]] float referenceOpacity(Kind kind, bool light) noexcept;

  // The material of `kind` for `settings`. light: the glass takes the light curve (a light scheme surface whose tone is
  // Scheme).
  [[nodiscard]] Material material(Kind kind, const Settings& settings, bool light) noexcept;

  // Shell popovers (menus, tooltips, window previews): the island glass; outside Liquid with the menu blur, so the backdrop does not
  // show through their text; the lens bends that blurred backdrop at the bezel.
  [[nodiscard]] Material popover(const Settings& settings, bool light) noexcept;

  // `t` of the way from a to b (a sheet that morphs from a shape).
  [[nodiscard]] inline Material mix(const Material& a, const Material& b, float t) noexcept {
    t = std::clamp(t, 0.0F, 1.0F);
    const float clarity = std::lerp(a.clarity, b.clarity, t);
    return {
        .tint = std::lerp(a.tint, b.tint, t), .level = std::lerp(a.level, b.level, t), .rim = std::lerp(a.rim, b.rim, t),
        .lens = std::lerp(a.lens, b.lens, t), .clarity = clarity, .blurStrength = blurStrength(clarity),
        .dense = std::lerp(a.dense, b.dense, t)
    };
  }

  // The tint for a configured opacity (relative to referenceOpacity); an opacity of 1 is solid.
  [[nodiscard]] float tintFor(const Material& m, Kind kind, bool light, float opacity) noexcept;

  // An opacity relative to tint_regular (0.70) rescaled to tint_thick (0.92), for sheets configured in regular terms.
  [[nodiscard]] constexpr float thickOpacity(float opacity) noexcept {
    return opacity >= 1.0F ? 1.0F : std::min(1.0F, opacity * (0.92F / 0.70F));
  }

  // Large glass adapts its density to the measured backdrop luminance (0 to 1) inside its tone: full density at or
  // past kAdaptLuminance[0] toward the tint's own tone, none from kAdaptLuminance[1]; it glides to each target over
  // kAdaptFadeMs and ignores moves within kAdaptDeadband, so it neither jumps nor pulses over video.
  // kAdaptFadeMs stays constexpr: callers derive constexpr steps from it.
  extern const float kAdaptDeadband;
  inline constexpr int kAdaptFadeMs = 600;
  [[nodiscard]] float adaptDensity(bool lightTint, float luminance) noexcept;

  // The tint for a backdrop of the tint's own tone, `density` (0 to 1) of the way to full density; `dense` is
  // Material::dense (denseLift).
  [[nodiscard]] inline float denser(float tint, float density, float dense) noexcept {
    return tint + std::clamp(dense, 0.0F, 1.0F) * (1.0F - tint) * std::clamp(density, 0.0F, 1.0F);
  }

} // namespace glass
