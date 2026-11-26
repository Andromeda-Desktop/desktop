#include "ui/surface_shadow.h"

#include "design_tokens.h"
#include "render/core/color.h"
#include "ui/palette.h"

#include <algorithm>
#include <cmath>

namespace ui::surface_shadow {

  std::int32_t blurRadius() noexcept { return static_cast<std::int32_t>(std::ceil(Tokens::elevationE3.contactBlur)); }

  bool enabled(bool componentShadow, const ShadowStyle& /*shadow*/) noexcept { return componentShadow; }

  Bleed bleed(bool componentShadow, const ShadowStyle& shadow) noexcept {
    if (!enabled(componentShadow, shadow)) {
      return {};
    }
    const auto offset = shadowDirectionOffset(shadow.direction);
    return {
        .left = blurRadius() + std::max(0, -offset.x),
        .right = blurRadius() + std::max(0, offset.x),
        .up = blurRadius() + std::max(0, -offset.y),
        .down = blurRadius() + std::max(0, offset.y),
    };
  }

  RoundedRectStyle
  style(const ShadowStyle& shadow, float backgroundOpacity, const Shape& shape) noexcept {
    const auto offset = shadowDirectionOffset(shadow.direction);
    // shell.shadow.alpha scales the e3 contact shadow; its default draws the token alpha.
    const float contact = isResolvedLightTheme() ? Tokens::elevationE3.contactLight : Tokens::elevationE3.contactDark;
    const float shadowAlpha = contact * std::clamp(shadow.alpha / ShadowStyle{}.alpha, 0.0F, 2.0F) *
                              std::clamp(backgroundOpacity, 0.0F, 1.0F);
    return RoundedRectStyle{
        .fill = rgba(0.0F, 0.0F, 0.0F, shadowAlpha),
        .border = Color{},
        .fillMode = FillMode::Solid,
        .corners = shape.corners,
        .logicalInset = shape.logicalInset,
        .radius = shape.radius,
        .softness = static_cast<float>(blurRadius()),
        .borderWidth = 0.0F,
        .outerShadow = OuterShadow::Drop,
        .shadowCutoutOffsetX = static_cast<float>(offset.x),
        .shadowCutoutOffsetY = static_cast<float>(offset.y),
    };
  }

  bool sameSurfaceMetrics(const ShadowStyle& previous, const ShadowStyle& next) noexcept {
    return previous.direction == next.direction;
  }

} // namespace ui::surface_shadow
