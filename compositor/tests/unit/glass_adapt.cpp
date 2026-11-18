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

// Random luminance sequences: the tone changes only past the band edges, and a sequence inside the band never flips.
ANDROMEDA_TEST(backdropToneHasHysteresis) {
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> any(0.0F, 1.0F);
  std::uniform_real_distribution<float> band(glass::kBackdropDark[0], std::nextafter(glass::kBackdropDark[1], 0.0F));
  for (int run = 0; run < 200; ++run) {
    bool dark = run % 2 == 0;
    for (int i = 0; i < 100; ++i) {
      const float l = any(rng);
      const bool next = glass::backdropDark(dark, l);
      CHECK(next == dark || (next ? l < glass::kBackdropDark[0] : l >= glass::kBackdropDark[1]));
      dark = next;
    }
    const bool held = dark;
    for (int i = 0; i < 100; ++i) {
      dark = glass::backdropDark(dark, band(rng));
    }
    CHECK(dark == held);
  }
}

// Every kind x style x toggle: only shapes flip, never in Frosted or with the toggle off, and Clear flips like Liquid.
ANDROMEDA_TEST(onlyShapesFlipInLiquidAndClear) {
  for (const auto kind : {glass::Kind::Shape, glass::Kind::Sheet, glass::Kind::Menu, glass::Kind::Tooltip}) {
    for (const bool adaptive : {false, true}) {
      for (const bool rt : {false, true}) {
        glass::Settings s;
        s.adaptiveTone = adaptive;
        s.reduceTransparency = rt;
        s.style = glass::Style::Frosted;
        CHECK(!glass::flipsWithBackdrop(kind, s));
        s.style = glass::Style::Liquid;
        const bool liquid = glass::flipsWithBackdrop(kind, s);
        s.style = glass::Style::Clear;
        CHECK_EQ(glass::flipsWithBackdrop(kind, s), liquid);
        CHECK(!liquid || (kind == glass::Kind::Shape && adaptive && !rt));
      }
    }
  }
  glass::Settings on;
  on.style = glass::Style::Clear;
  CHECK(glass::flipsWithBackdrop(glass::Kind::Shape, on));
}

int main() { return RUN_TESTS(); }
