#pragma once

// The Glass style and slider mapping (CLAUDE.md Decisions "Glass slider", "One glass system"), header-only so the
// shell (through libdesk-ui Style::glassMaterial) and the compositor use the same values. No other code maps them.

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

  // Over a low-contrast backdrop each style's glass moves this fraction of its gap to opaque, in its own colour
  // (concept C styles.css --dense), indexed by Style: {light glass, dark glass}.
  inline constexpr float kDenseOverlay[4][2] = {{0.0F, 0.2F}, {0.0F, 0.66F}, {0.66F, 0.66F}, {0.3F, 0.45F}};
  // The least alpha at full density that keeps label_1 at 4.5:1 over a white (dark glass) or black (light glass)
  // backdrop, blended in sRGB like the compositor: {light glass, dark glass}.
  inline constexpr float kLegibleAlpha[2] = {0.60F, 0.80F};
  // Ink alpha label_2 (4.5:1) and label_3 (3:1, dim by design) reach at full density over kLegibleAlpha glass:
  // {light glass (dark ink), dark glass (light ink)}. Secondary ink rises to these with the surface's density.
  inline constexpr float kLegibleInk2[2] = {0.80F, 0.86F};
  inline constexpr float kLegibleInk3[2] = {0.62F, 0.60F};

  // The palette a kind's glass and content take. Dark: dark glass with white content in both schemes.
  enum class Tone : std::uint8_t { Scheme, Dark };

  // Clarity 0 blurs at full size, 1 not at all: Frosted stops at 0.7 (a 30 % blur); Liquid spans 0.85 (Soft) to 0.45
  // (Strong): every Liquid surface keeps a real blur, like Apple's Regular glass.
  inline constexpr float kLiquidClarity[2] = {0.85F, 0.45F};
  // Liquid sheets and menus sit over windows and text, so they blur about 4x a shape at the default (Soft 0.30 to
  // Strong 0): text under them does not show through.
  inline constexpr float kLiquidSheetClarity[2] = {0.30F, 0.0F};
  // Liquid's blur recipe lifts the backdrop by this brightness factor (no frost dimming, no grain).
  inline constexpr float kLiquidLift = 1.05F;
  // Glass shapes of one surface with the same material join by a smooth union within this many logical px, so shapes
  // that move together merge and split like one liquid (gaps under about half of it bridge).
  inline constexpr float kMergeReach = 24.0F;
  // Glass rims within this many logical px of the pointer turn their key light toward it.
  inline constexpr float kPointerLightReach = 128.0F;
  [[nodiscard]] constexpr float blurStrength(float clarity) noexcept { return 1.0F - clarity; }

  // Frosted: blur and tint from the Tint and Blur sliders, an optional specular edge. Liquid: a soft blur with a lift, a
  // convex lens bezel at the edge. Clear: Liquid with kClearTint, white content, and dimming from the density path
  // (adaptDensity) over bright backdrops, so text keeps 4.5:1. Tinted: Frosted at the design tint and blur (kTintedLevel).
  // The values are the portal's glass-style numbers.
  enum class Style : std::uint8_t { Frosted, Liquid, Clear, Tinted };

  // The Tint and Blur slider position Tinted glass takes.
  inline constexpr float kTintedLevel = 0.5F;

  // Shape tints at the lowest and the middle (kTintedLevel) Tint slider position: {light glass, dark glass}.
  inline constexpr float kLowestTint[2] = {0.20F, 0.30F};
  inline constexpr float kTintedTint[2] = {0.34F, 0.56F};
  // The tint alpha of Clear glass: enough to carry a tint colour, so the compositor can dim through the density.
  inline constexpr float kClearTint = 0.04F;

  // Material::dense: the fraction of the gap to opaque every tint closes at full density; the style's overlay, raised
  // so the style's lowest Shape tint reaches kLegibleAlpha. It stays below 1, so the slider order holds. Tooltips close half.
  [[nodiscard]] inline float denseLift(Kind kind, Style style, bool lightGlass) noexcept {
    const int tone = lightGlass ? 0 : 1;
    const float lowest = style == Style::Clear ? kClearTint : style == Style::Tinted ? kTintedTint[tone] : kLowestTint[tone];
    const float lift = std::max(kDenseOverlay[static_cast<int>(style)][tone], (kLegibleAlpha[tone] - lowest) / (1.0F - lowest));
    return kind == Kind::Tooltip ? 0.5F * lift : lift;
  }

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

    [[nodiscard]] float activeTint() const noexcept {
      switch (style) {
      case Style::Liquid: return liquidTint;
      case Style::Clear: return 0.0F;
      case Style::Tinted: return kTintedLevel;
      case Style::Frosted: break;
      }
      return tint;
    }
    [[nodiscard]] float activeFrost() const noexcept { return style == Style::Tinted ? kTintedLevel : frost; }
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
  [[nodiscard]] constexpr float referenceOpacity(Kind kind, bool light) noexcept {
    return kind == Kind::Shape ? 0.70F : light ? 0.88F : 0.92F;
  }

  // The material of `kind` for `settings`. light: the glass takes the light curve (a light scheme surface whose tone is
  // Scheme).
  [[nodiscard]] inline Material material(Kind kind, const Settings& settings, bool light) noexcept {
    if (settings.reduceTransparency) {
      return {};
    }
    const auto at = [](float v, float low, float mid, float high) {
      v = std::clamp(v, 0.0F, 1.0F);
      return v < 0.5F ? std::lerp(low, mid, v * 2.0F) : std::lerp(mid, high, v * 2.0F - 1.0F);
    };
    const float g = std::clamp(settings.activeTint(), 0.0F, 1.0F);
    const auto curve = [&](float clear, float tinted, float frosted) { return at(g, clear, tinted, frosted); };
    float tint = 0.0F;
    switch (kind) {
    case Kind::Shape:
      tint = light ? curve(kLowestTint[0], kTintedTint[0], 0.84F) : curve(kLowestTint[1], kTintedTint[1], 0.70F);
      break;
    case Kind::Sheet:
    case Kind::Menu:
      tint = light ? curve(0.60F, 0.84F, 0.97F) : curve(0.46F, 0.66F, 0.99F);
      break;
    case Kind::Tooltip:
      tint = light ? curve(0.70F, 0.84F, 0.95F) : curve(0.80F, 0.88F, 0.99F);
      break;
    }
    const bool shape = kind == Kind::Shape;
    if (settings.style == Style::Clear) {
      tint = kClearTint;
    }
    if (settings.style == Style::Liquid || settings.style == Style::Clear) {
      const bool sheet = settings.style == Style::Liquid && (kind == Kind::Sheet || kind == Kind::Menu);
      const float* range = sheet ? kLiquidSheetClarity : kLiquidClarity;
      const float clarity = std::lerp(range[0], range[1], std::clamp(settings.liquidBlur, 0.0F, 1.0F));
      const float level = std::lerp(0.1F, 1.0F, std::clamp(settings.lens, 0.0F, 1.0F));
      return {
          .tint = tint, .level = level, .rim = settings.edgeReflection ? at(settings.lens, 0.4F, 1.0F, 1.0F) : 0.0F, .lens = level, .clarity = clarity,
          .blurStrength = blurStrength(clarity), .dense = denseLift(kind, settings.style, light && settings.style != Style::Clear)
      };
    }
    // Sheets blur a step less than shapes at the middle and match them at both ends.
    const float frost = settings.activeFrost();
    const float clarity = 0.7F * (shape ? at(frost, 1.0F, 0.5F, 0.0F) : at(frost, 1.0F, 0.25F, 0.0F));
    return {
        .tint = tint, .level = 0.0F, .rim = settings.edgeReflection ? 1.0F : 0.0F, .lens = 0.0F, .clarity = clarity,
        .blurStrength = blurStrength(clarity), .dense = denseLift(kind, settings.style, light && settings.style != Style::Clear)
    };
  }

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
  [[nodiscard]] inline float tintFor(const Material& m, Kind kind, bool light, float opacity) noexcept {
    return opacity >= 1.0F || m.tint >= 1.0F ? 1.0F : std::min(1.0F, m.tint * opacity / referenceOpacity(kind, light));
  }

  // An opacity relative to tint_regular (0.70) rescaled to tint_thick (0.92), for sheets configured in regular terms.
  [[nodiscard]] constexpr float thickOpacity(float opacity) noexcept {
    return opacity >= 1.0F ? 1.0F : std::min(1.0F, opacity * (0.92F / 0.70F));
  }

  // Large glass adapts its density to the measured backdrop luminance (0 to 1) inside its tone: full density at or
  // past kAdaptLuminance[0] toward the tint's own tone, none from kAdaptLuminance[1]; it glides to each target over
  // kAdaptFadeMs and ignores moves within kAdaptDeadband, so it neither jumps nor pulses over video.
  inline constexpr float kAdaptLuminance[2] = {0.2F, 0.75F};
  inline constexpr float kAdaptDeadband = 0.04F;
  inline constexpr int kAdaptFadeMs = 600;
  [[nodiscard]] inline float adaptDensity(bool lightTint, float luminance) noexcept {
    const float own = lightTint ? luminance : 1.0F - luminance;
    const float x = std::clamp((kAdaptLuminance[1] - own) / (kAdaptLuminance[1] - kAdaptLuminance[0]), 0.0F, 1.0F);
    return x * x * (3.0F - 2.0F * x);
  }

  // The tint for a backdrop of the tint's own tone, `density` (0 to 1) of the way to full density; `dense` is
  // Material::dense (denseLift).
  [[nodiscard]] inline float denser(float tint, float density, float dense) noexcept {
    return tint + std::clamp(dense, 0.0F, 1.0F) * (1.0F - tint) * std::clamp(density, 0.0F, 1.0F);
  }

} // namespace glass
