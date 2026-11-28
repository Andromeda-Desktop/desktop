#include "glass_material.h"

namespace glass {

  namespace {
  // Over a low-contrast backdrop each style's glass moves at least this fraction of its gap to opaque, in its own
  // colour, by Style: {light glass, dark glass}. A light step only: each style keeps its slider's look over any backdrop.
  constexpr float kDenseOverlay[4][2] = {{0.0F, 0.2F}, {0.15F, 0.25F}, {0.25F, 0.25F}, {0.3F, 0.45F}};
  // Clarity 0 blurs at full size, 1 not at all: Frosted stops at 0.7 (a 30 % blur); Liquid spans 0.85 (Soft) to 0.45
  // (Strong): every Liquid surface keeps a real blur, like Apple's Regular glass.
  constexpr float kLiquidClarity[2] = {0.85F, 0.45F};
  // Liquid sheets and menus sit over windows and text, so they blur about 4x a shape at the default (Soft 0.30 to
  // Strong 0): text under them does not show through.
  constexpr float kLiquidSheetClarity[2] = {0.30F, 0.0F};
  // Specular rim strength of frosted glass with edge reflection on: a soft glow that marks the edge without a line.
  constexpr float kFrostedRim = 0.6F;
  // The Tint and Blur slider position Tinted glass takes.
  constexpr float kTintedLevel = 0.5F;
  // Backdrop luminance of full density (toward the tint's own tone) and of none.
  constexpr float kAdaptLuminance[2] = {0.2F, 0.75F};
  } // namespace

  const float kLegibleInk2[2] = {0.80F, 0.86F};
  const float kLegibleInk3[2] = {0.62F, 0.60F};
  const float kGlassInk1 = 1.0F;
  const float kGlassInk2 = 0.75F;
  const float kVibrancy[2] = {1.0F, 0.0F};
  const float kMergeReach = 24.0F;
  const float kPointerLightReach = 128.0F;
  const float kLowestTint[2] = {0.20F, 0.30F};
  const float kTintedTint[2] = {0.34F, 0.56F};
  const float kClearTint = 0.04F;
  const float kLiquidTint[2] = {0.10F, 0.16F};
  const float kAdaptDeadband = 0.04F;

  float denseLift(Kind kind, Style style, bool lightGlass) noexcept {
    const float lift = kDenseOverlay[static_cast<int>(style)][lightGlass ? 0 : 1];
    return kind == Kind::Tooltip ? 0.5F * lift : lift;
  }

  float Settings::activeTint() const noexcept {
      switch (style) {
      case Style::Liquid: return liquidTint;
      case Style::Clear: return 0.0F;
      case Style::Tinted: return kTintedLevel;
      case Style::Frosted: break;
      }
      return tint;
    }
  float Settings::activeFrost() const noexcept { return style == Style::Tinted ? kTintedLevel : frost; }

  float referenceOpacity(Kind kind, bool light) noexcept {
    return kind == Kind::Shape ? 0.70F : light ? 0.88F : 0.92F;
  }

  Material material(Kind kind, const Settings& settings, bool light) noexcept {
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
      if (settings.style == Style::Liquid) {
        tint = light ? curve(kClearTint, kLiquidTint[0], 0.84F) : curve(kClearTint, kLiquidTint[1], 0.70F);
      } else {
        tint = light ? curve(kLowestTint[0], kTintedTint[0], 0.84F) : curve(kLowestTint[1], kTintedTint[1], 0.70F);
      }
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
        .tint = tint, .level = 0.0F, .rim = settings.edgeReflection ? kFrostedRim : 0.0F, .lens = 0.0F, .clarity = clarity,
        .blurStrength = blurStrength(clarity), .dense = denseLift(kind, settings.style, light && settings.style != Style::Clear)
    };
  }

  Material popover(const Settings& settings, bool light) noexcept {
    Material m = material(Kind::Shape, settings, light);
    const Material menu = material(Kind::Menu, settings, light);
    // Liquid keeps the island blur, so backdrop shapes and the lens bend show; the near-black dark tint keeps text legible.
    if (settings.style != Style::Liquid) {
      m.clarity = menu.clarity;
      m.blurStrength = menu.blurStrength;
    }
    return m;
  }

  float tintFor(const Material& m, Kind kind, bool light, float opacity) noexcept {
    return opacity >= 1.0F || m.tint >= 1.0F ? 1.0F : std::min(1.0F, m.tint * opacity / referenceOpacity(kind, light));
  }

  float adaptDensity(bool lightTint, float luminance) noexcept {
    const float own = lightTint ? luminance : 1.0F - luminance;
    const float x = std::clamp((kAdaptLuminance[1] - own) / (kAdaptLuminance[1] - kAdaptLuminance[0]), 0.0F, 1.0F);
    return x * x * (3.0F - 2.0F * x);
  }

} // namespace glass
