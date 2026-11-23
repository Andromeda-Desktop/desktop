#pragma once

#include <algorithm>
#include <cmath>

namespace andromeda {
  // How a fading window, layer or close snapshot is drawn: alpha on its own buffers, or captured and drawn by an effect.
  enum class EffectDraw { PerBuffer, Composited };

  // What a fade's draw depends on; callers state the facts, effectDraw picks the path.
  struct FadeFacts {
    bool animating = false;
    // A user effect preset is bound for the event.
    bool customEffect = false;
    // The built-in fade shader applies to the event.
    bool builtinEffect = false;
    // Alpha on the buffers gives the captured fade's pixels: one opaque surface that covers everything it draws.
    bool alphaEquivalent = false;
    // The built-in slide style moves the content.
    bool slideStyle = false;
  };

  [[nodiscard]] constexpr EffectDraw effectDraw(const FadeFacts& facts) {
    if (!facts.animating) {
      return EffectDraw::PerBuffer;
    }
    if (facts.customEffect || (facts.builtinEffect && !facts.alphaEquivalent)) {
      return EffectDraw::Composited;
    }
    return EffectDraw::PerBuffer;
  }

  // The built-in slide runs: it keeps per-buffer alpha, as its opacity curve differs from the lifecycle progress.
  [[nodiscard]] constexpr bool effectSlides(const FadeFacts& facts) {
    return facts.animating && facts.slideStyle && !facts.customEffect && effectDraw(facts) == EffectDraw::PerBuffer;
  }

  // The fade value of a raw progress, clamped to [0, 1] against overshooting curves: a slide keeps the content visible
  // through more of its travel so it reads as motion, not a fade.
  [[nodiscard]] inline float fadeCurve(const FadeFacts& facts, float raw) {
    const float alpha = std::clamp(raw, 0.0F, 1.0F);
    return effectSlides(facts) ? std::sqrt(alpha) : alpha;
  }

  // The alpha the buffers take: 1 when an effect draws the fade from captured pixels.
  [[nodiscard]] inline float fadeAlpha(const FadeFacts& facts, float raw) {
    return effectDraw(facts) == EffectDraw::Composited ? 1.0F : fadeCurve(facts, raw);
  }
} // namespace andromeda
