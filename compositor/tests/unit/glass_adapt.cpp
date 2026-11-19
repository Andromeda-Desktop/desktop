#include "protocols/glass_material.h"

#include "check.h"

// Every kind, style, tone, tint and density: the density never lowers the tint, keeps the slider order, and lifts
// every tint from the style's lowest up to at least kLegibleAlpha at full density.
ANDROMEDA_TEST(densityKeepsSliderOrderAndLegibilityFloor) {
  constexpr int kSteps = 20;
  for (const auto kind : {glass::Kind::Shape, glass::Kind::Sheet, glass::Kind::Menu, glass::Kind::Tooltip}) {
    for (const auto style : {glass::Style::Frosted, glass::Style::Liquid, glass::Style::Clear, glass::Style::Tinted}) {
      for (const bool light : {true, false}) {
        const float dense = glass::denseLift(kind, style, light);
        const int tone = light ? 0 : 1;
        const float lowest = style == glass::Style::Clear ? glass::kClearTint
            : style == glass::Style::Tinted                ? glass::kTintedTint[tone]
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
            if (kind != glass::Kind::Tooltip && d == kSteps) {
              CHECK(out >= glass::kLegibleAlpha[tone] - 1e-5F);
            }
            previous = out;
          }
        }
      }
    }
  }
}

int main() { return RUN_TESTS(); }
