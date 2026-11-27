#include "render/animation/animation_manager.h"
#include "render/scene/node.h"
#include "ui/controls/box.h"
#include "ui/palette.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <print>
#include <random>
#include <string>

namespace {

  bool g_ok = true;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::println(stderr, "palette_tone_test: FAIL: {}", message);
      g_ok = false;
    }
  }

  std::unique_ptr<Box> labelBox() {
    auto box = std::make_unique<Box>();
    box->setFill(colorSpecFromRole(ColorRole::Label1));
    return box;
  }

} // namespace

int main() {
  const Palette light = tokenPalette(true);
  const Palette dark = tokenPalette(false);
  setDarkPalette(dark);
  setPalette(light);
  expect(light.label1 != dark.label1, "light and dark label_1 differ");

  Node darkRoot;
  darkRoot.setPaletteTone(PaletteTone::Dark);
  Box* attached = static_cast<Box*>(darkRoot.addChild(labelBox()));
  expect(attached->style().fill == dark.label1, "a box added under a Dark root resolves the dark label_1");

  Node schemeRoot;
  Box* plain = static_cast<Box*>(schemeRoot.addChild(labelBox()));
  expect(plain->style().fill == light.label1, "a box under a Scheme root resolves the scheme label_1");

  schemeRoot.setPaletteTone(PaletteTone::Dark);
  expect(plain->style().fill == dark.label1, "a root that turns Dark re-resolves its children");
  schemeRoot.setPaletteTone(std::nullopt);
  expect(plain->style().fill == light.label1, "a root that drops its tone returns to the scheme");

  setPalette(dark);
  setPalette(light);
  expect(attached->style().fill == dark.label1, "a palette change keeps the Dark subtree dark");

  auto moved = darkRoot.removeChild(attached);
  Box* landed = static_cast<Box*>(schemeRoot.addChild(std::move(moved)));
  expect(landed->style().fill == light.label1, "a box moved from a Dark root to a Scheme root resolves again");
  auto back = schemeRoot.removeChild(landed);
  attached = static_cast<Box*>(darkRoot.addChild(std::move(back)));
  expect(attached->style().fill == dark.label1, "a box moved back under the Dark root resolves the dark label_1");

  // Model: random fades, reversals, snaps and ticks. Invariants in every reachable state: the fill lies between the
  // dark and light label_1; a fade or a reversal never jumps the fill; a settled subtree has its tone's exact colour.
  setLightPalette(light);
  AnimationManager animations;
  Node fadeRoot;
  fadeRoot.setAnimationManager(&animations);
  fadeRoot.setPaletteTone(PaletteTone::Light);
  Box* faded = static_cast<Box*>(fadeRoot.addChild(labelBox()));
  const auto between = [&](const Color& c) {
    const auto in = [](float v, float a, float b) { return v >= std::min(a, b) - 1e-4F && v <= std::max(a, b) + 1e-4F; };
    return in(c.r, dark.label1.r, light.label1.r) && in(c.g, dark.label1.g, light.label1.g)
        && in(c.b, dark.label1.b, light.label1.b) && in(c.a, dark.label1.a, light.label1.a);
  };
  const auto distance = [](const Color& a, const Color& b) {
    return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b), std::abs(a.a - b.a)});
  };
  const desk::motion::SpringConfig spring{.damping = 1.0, .stiffness = 1542.1};
  std::mt19937 random(7);
  auto clock = std::chrono::steady_clock::now();
  const auto advance = [&](int ms) {
    clock += std::chrono::milliseconds(ms);
    animations.tickAt(clock);
  };
  for (int step = 0; step < 4000; ++step) {
    const Color before = faded->style().fill;
    const int event = static_cast<int>(random() % 4);
    if (event == 0 || event == 1) {
      fadeRoot.fadePaletteTone(event == 0 ? PaletteTone::Dark : PaletteTone::Light, spring);
      expect(distance(before, faded->style().fill) < 1e-4F, "a fade or reversal does not jump the colour");
    } else if (event == 2 && random() % 8 == 0) {
      fadeRoot.setPaletteTone(random() % 2 == 0 ? PaletteTone::Dark : PaletteTone::Light);
    } else {
      advance(static_cast<int>(random() % 40));
    }
    expect(between(faded->style().fill), "the fill stays between the dark and light label_1");
    if (!fadeRoot.paletteToneFade().has_value()) {
      const Color& want = fadeRoot.paletteTone() == PaletteTone::Dark ? dark.label1 : light.label1;
      expect(faded->style().fill == want, "a settled subtree has its tone's colour");
    }
    if (!g_ok) {
      std::println(stderr, "palette_tone_test: model failed at step {}", step);
      break;
    }
  }
  for (int i = 0; i < 200 && fadeRoot.paletteToneFade().has_value(); ++i) {
    advance(16);
  }
  expect(!fadeRoot.paletteToneFade().has_value(), "a fade settles");

  return g_ok ? 0 : 1;
}
