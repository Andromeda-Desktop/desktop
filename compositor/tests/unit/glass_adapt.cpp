#include "protocols/glass_material.h"

#include "check.h"

// Every kind, style, tone, tint and density: the density never lowers the tint and keeps the slider order.
ANDROMEDA_TEST(densityKeepsSliderOrderAndLegibilityFloor) {
  constexpr int kSteps = 20;
  for (const auto kind : {glass::Kind::Shape, glass::Kind::Sheet, glass::Kind::Menu, glass::Kind::Tooltip}) {
    for (const auto style : {glass::Style::Frosted, glass::Style::Liquid, glass::Style::Clear, glass::Style::Tinted}) {
      for (const bool light : {true, false}) {
        const float dense = glass::denseLift(kind, style, light);
        const int tone = light ? 0 : 1;
        const bool liquidShape = style == glass::Style::Liquid && kind == glass::Kind::Shape;
        const float lowest = style == glass::Style::Clear || liquidShape ? glass::kClearTint
            : style == glass::Style::Tinted                              ? glass::kTintedTint[tone]
                                                                         : glass::kLowestTint[tone];
        for (int d = 0; d <= kSteps; ++d) {
          const float density = static_cast<float>(d) / kSteps;
          float previous = -1.0F;
          for (int i = 0; i <= kSteps; ++i) {
            const float tint = std::lerp(lowest, 0.99F, static_cast<float>(i) / kSteps);
            const float out = glass::denser(tint, density, dense);
            CHECK(out >= tint);
            CHECK(out <= 1.0F);
            CHECK(out > previous);
            previous = out;
          }
        }
      }
    }
  }
}

// Under Clear an app window blurs by the Clear Blur slider, the only one Clear shows; the hidden Frosted Blur is ignored.
ANDROMEDA_TEST(clearWindowBlurFollowsClearSlider) {
  for (const float hidden : {0.0F, 1.0F}) {
    glass::Settings sharp{.style = glass::Style::Clear, .frost = hidden, .liquidBlur = 0.0F};
    glass::Settings soft{.style = glass::Style::Clear, .frost = hidden, .liquidBlur = 1.0F};
    const float sharpBlur = glass::material(glass::Kind::Sheet, glass::windowSettings(sharp), false).blurStrength;
    const float softBlur = glass::material(glass::Kind::Sheet, glass::windowSettings(soft), false).blurStrength;
    CHECK(sharpBlur < 0.5F);
    CHECK(softBlur > sharpBlur);
  }
}

int main() { return RUN_TESTS(); }
