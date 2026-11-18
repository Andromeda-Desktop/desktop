#include "protocols/glass_material.h"

#include "check.h"

#include <random>

// Every reachable (tint, density, dense): the density never lowers the tint, keeps the slider order, and lifts every
// tint from kDenseFrom up to at least kDenseTint at full density.
ANDROMEDA_TEST(densityKeepsSliderOrderAndLegibilityFloor) {
  constexpr int kSteps = 20;
  for (const float dense : {0.0F, 0.5F, 1.0F}) {
    for (int d = 0; d <= kSteps; ++d) {
      const float density = static_cast<float>(d) / kSteps;
      float previous = -1.0F;
      for (int i = 1; i <= kSteps; ++i) {
        const float tint = static_cast<float>(i) / kSteps;
        const float out = glass::denser(tint, density, dense);
        CHECK(out >= tint);
        CHECK(out <= 1.0F);
        CHECK(out > previous);
        if (dense == 1.0F && density == 1.0F && tint >= glass::kDenseFrom) {
          CHECK(out >= glass::kDenseTint - 1e-5F);
        }
        previous = out;
      }
    }
  }
}

int main() { return RUN_TESTS(); }
