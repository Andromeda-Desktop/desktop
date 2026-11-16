#include "ui/contrast.h"
#include "ui/theme_palette.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>

namespace {

int failures = 0;
unsigned seed = 0;
int step = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s (seed %u step %d)\n", __FILE__, __LINE__, #c, seed, step); ++failures; } } while (0)

constexpr double kEps = 1e-6;

void checkInvariants(const ThemeAnchors& anchors) {
  const ThemePalette p = deriveThemePalette(anchors);
  const double reach = std::max(contrastRatio(rgbHex(0xffffff), p.view), contrastRatio(rgbHex(0x000000), p.view));
  const double text = std::min(7.0, reach);
  CHECK(p.view.r == anchors.surface.r && p.view.g == anchors.surface.g && p.view.b == anchors.surface.b);
  CHECK(contrastRatio(p.label1, p.view) >= text - kEps);
  CHECK(contrastRatio(p.label2, p.view) >= std::min(4.5, reach) - kEps);
  CHECK(contrastRatio(p.label2, p.view) <= contrastRatio(p.label1, p.view) + kEps);
  CHECK(contrastRatio(p.accent, p.view) >= std::min(3.0, reach) - kEps);
  for (int i = 1; i < 16; ++i) {
    if (i != 8) {
      CHECK(contrastRatio(p.ansi[i], p.view) >= text - kEps);
    }
  }
  CHECK(p.ansi[8] == p.label2);
  double low = 1.0;
  double high = 0.0;
  for (int i = 1; i <= 6; ++i) {
    const Oklch c = toOklch(p.ansi[i]);
    low = std::min(low, c.lightness);
    high = std::max(high, c.lightness);
    CHECK(c.chroma <= 0.18);
  }
  // Below L 0.1 one 8-bit step moves L by up to 0.07, so the ring is black within precision.
  CHECK(high < 0.1 || high - low <= 0.01);
}

// The scene tint: the smallest 0.01 step at or above the floor where label 2 keeps 4.5:1 over every backdrop colour.
void checkTint(const ThemeAnchors& anchors, const std::array<Color, 8>& backdrop, float floor) {
  const ThemePalette p = deriveThemePalette(anchors);
  const float t = tintForContrast(p.label2, p.view, backdrop, floor);
  const auto holds = [&](float a) {
    return std::ranges::all_of(backdrop, [&](const Color& b) {
      const Color over{p.view.r * a + b.r * (1.0F - a), p.view.g * a + b.g * (1.0F - a), p.view.b * a + b.b * (1.0F - a), 1.0F};
      return contrastRatio(p.label2, over) >= 4.5;
    });
  };
  CHECK(t >= floor - 1e-4F && t <= 1.0F);
  if (t < 1.0F) {
    CHECK(holds(t));
  }
  if (t - 0.01F >= floor) {
    CHECK(!holds(t - 0.01F));
  }
}

} // namespace

int main() {
  const unsigned seeds[] = {1, 2, 3, 0xC0FFEE};
  for (const unsigned s : seeds) {
    seed = s;
    std::mt19937 rng(s);
    std::uniform_real_distribution<float> unit(0.0F, 1.0F);
    auto color = [&] {
      auto byte = [&] { return std::round(unit(rng) * 255.0F) / 255.0F; };
      return Color{byte(), byte(), byte(), 1.0F};
    };
    for (step = 0; step < 300; ++step) {
      checkInvariants({.surface = color(), .text = color(), .accent = color()});
      std::array<Color, 8> backdrop{};
      std::ranges::generate(backdrop, color);
      checkTint({.surface = color(), .text = color(), .accent = color()}, backdrop, std::round(unit(rng) * 80.0F) / 100.0F);
    }
  }
  seed = 0;
  step = -1;
  for (const std::uint32_t surface : {0x000000U, 0xffffffU, 0x777777U, 0x18181bU}) {
    checkInvariants({.surface = rgbHex(surface), .text = rgbHex(surface), .accent = rgbHex(surface)});
  }
  if (failures != 0) {
    std::fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  return 0;
}
