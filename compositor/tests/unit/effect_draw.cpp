#include "scene/effect_draw.h"

#include "check.h"

using namespace andromeda;

// Every combination of fade facts and a range of raw progress values, including overshoot.
ANDROMEDA_TEST(effectDrawInvariantsHoldForEveryFactCombination) {
  for (int bits = 0; bits < 32; ++bits) {
    const FadeFacts f{
        .animating = (bits & 1) != 0,
        .customEffect = (bits & 2) != 0,
        .builtinEffect = (bits & 4) != 0,
        .alphaEquivalent = (bits & 8) != 0,
        .slideStyle = (bits & 16) != 0,
    };
    const EffectDraw draw = effectDraw(f);
    if (!f.animating) {
      CHECK(draw == EffectDraw::PerBuffer);
      CHECK(!effectSlides(f));
    }
    if (f.animating && f.customEffect) {
      CHECK(draw == EffectDraw::Composited);
    }
    if (f.animating && f.builtinEffect && !f.alphaEquivalent) {
      CHECK(draw == EffectDraw::Composited);
    }
    if (!f.customEffect && (f.alphaEquivalent || !f.builtinEffect)) {
      CHECK(draw == EffectDraw::PerBuffer);
    }
    if (effectSlides(f)) {
      CHECK(draw == EffectDraw::PerBuffer);
    }
    for (int i = -2; i <= 12; ++i) {
      const float raw = static_cast<float>(i) / 10.0F;
      const float alpha = fadeAlpha(f, raw);
      CHECK(alpha >= 0.0F && alpha <= 1.0F);
      if (draw == EffectDraw::Composited) {
        CHECK(alpha == 1.0F);
      } else if (effectSlides(f)) {
        CHECK(alpha >= std::clamp(raw, 0.0F, 1.0F));
      } else {
        CHECK(alpha == std::clamp(raw, 0.0F, 1.0F));
      }
    }
  }
}

int main() { return RUN_TESTS(); }
