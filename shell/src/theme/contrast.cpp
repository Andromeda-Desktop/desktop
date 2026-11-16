#include "theme/contrast.h"

#include "ui/contrast.h"

#include <cmath>

namespace noctalia::theme {

  namespace {

    ::Color toUi(const Color& c) { return ::rgba(static_cast<float>(c.r) / 255.0F, static_cast<float>(c.g) / 255.0F, static_cast<float>(c.b) / 255.0F); }

    Color toTheme(const ::Color& c) {
      auto byte = [](float v) { return static_cast<int>(std::lround(v * 255.0F)); };
      return Color(byte(c.r), byte(c.g), byte(c.b));
    }

  } // namespace

  double relativeLuminance(int r, int g, int b) { return ::relativeLuminance(toUi(Color(r, g, b))); }

  double contrastRatio(const Color& a, const Color& b) { return ::contrastRatio(toUi(a), toUi(b)); }

  bool isDark(const Color& c) { return relativeLuminance(c.r, c.g, c.b) < 0.179; }

  Color ensureContrast(const Color& foreground, const Color& background, double minRatio, int preferLight) {
    return toTheme(::ensureContrast(toUi(foreground), toUi(background), minRatio, preferLight));
  }

} // namespace noctalia::theme
