#include "shell/bar/bar_ink.h"

#include <algorithm>
#include <cstdio>
#include <print>
#include <random>

// Drives random luminance walks around the flip point and checks the ink after every sample.
int main() {
  constexpr float kFlip = 0.15F;
  constexpr float kBand = 0.04F;
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> step(-0.06F, 0.06F);
  for (int walk = 0; walk < 200; ++walk) {
    BarInk ink = BarInk::Light;
    float luminance = std::uniform_real_distribution<float>(0.0F, 1.0F)(rng);
    for (int i = 0; i < 400; ++i) {
      luminance = std::clamp(luminance + step(rng), 0.0F, 1.0F);
      const BarInk next = nextBarInk(ink, luminance, kFlip, kBand);
      if (luminance > kFlip + kBand && next != BarInk::Dark) {
        std::println(stderr, "bar_ink_test: a bright backdrop {} must give dark ink", luminance);
        return 1;
      }
      if (luminance < kFlip - kBand && next != BarInk::Light) {
        std::println(stderr, "bar_ink_test: a dark backdrop {} must give white ink", luminance);
        return 1;
      }
      if (luminance >= kFlip - kBand && luminance <= kFlip + kBand && next != ink) {
        std::println(stderr, "bar_ink_test: ink flipped inside the deadband at {}", luminance);
        return 1;
      }
      ink = next;
    }
  }
  if (barInkTone(BarInk::Light) == barInkTone(BarInk::Dark)) {
    std::println(stderr, "bar_ink_test: the two inks must map to different palettes");
    return 1;
  }
  return 0;
}
