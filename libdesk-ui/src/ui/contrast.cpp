#include "ui/contrast.h"

#include <algorithm>
#include <cmath>

namespace {

  struct LinearRgb {
    double red;
    double green;
    double blue;
  };

  double linearize(double channel) {
    return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
  }

  double delinearize(double channel) {
    return channel <= 0.0031308 ? channel * 12.92 : 1.055 * std::pow(channel, 1.0 / 2.4) - 0.055;
  }

  LinearRgb toLinear(const Oklch& color) {
    const double a = color.chroma * std::cos(color.hueRadians);
    const double b = color.chroma * std::sin(color.hueRadians);
    const double lRoot = color.lightness + 0.3963377774 * a + 0.2158037573 * b;
    const double mRoot = color.lightness - 0.1055613458 * a - 0.0638541728 * b;
    const double sRoot = color.lightness - 0.0894841775 * a - 1.2914855480 * b;
    const double l = lRoot * lRoot * lRoot;
    const double m = mRoot * mRoot * mRoot;
    const double s = sRoot * sRoot * sRoot;
    return {
        .red = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
        .green = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
        .blue = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s,
    };
  }

  bool inGamut(const LinearRgb& color) {
    constexpr double tolerance = 1e-7;
    return color.red >= -tolerance && color.red <= 1.0 + tolerance && color.green >= -tolerance &&
           color.green <= 1.0 + tolerance && color.blue >= -tolerance && color.blue <= 1.0 + tolerance;
  }

  Color quantize(const LinearRgb& color) {
    auto channel = [](double linear) {
      return static_cast<float>(std::round(std::clamp(delinearize(linear), 0.0, 1.0) * 255.0) / 255.0);
    };
    return Color{channel(color.red), channel(color.green), channel(color.blue), 1.0F};
  }

  double luminance(const Color& c) {
    return 0.2126 * linearize(c.r) + 0.7152 * linearize(c.g) + 0.0722 * linearize(c.b);
  }

} // namespace

Oklch toOklch(const Color& color) {
  const double red = linearize(color.r);
  const double green = linearize(color.g);
  const double blue = linearize(color.b);
  const double l = std::cbrt(0.4122214708 * red + 0.5363325363 * green + 0.0514459929 * blue);
  const double m = std::cbrt(0.2119034982 * red + 0.6806995451 * green + 0.1073969566 * blue);
  const double s = std::cbrt(0.0883024619 * red + 0.2817188376 * green + 0.6299787005 * blue);
  const double a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
  const double b = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
  return {.lightness = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
          .chroma = std::hypot(a, b),
          .hueRadians = std::atan2(b, a)};
}

Color fromOklch(const Oklch& color) {
  const LinearRgb linear = toLinear(color);
  if (inGamut(linear)) {
    return quantize(linear);
  }
  double low = 0.0;
  double high = color.chroma;
  Oklch probe = color;
  probe.chroma = 0.0;
  LinearRgb best = toLinear(probe);
  for (int i = 0; i < 20; ++i) {
    probe.chroma = (low + high) / 2.0;
    const LinearRgb candidate = toLinear(probe);
    if (inGamut(candidate)) {
      low = probe.chroma;
      best = candidate;
    } else {
      high = probe.chroma;
    }
  }
  return quantize(best);
}

double contrastRatio(const Color& a, const Color& b) {
  const double l1 = luminance(a);
  const double l2 = luminance(b);
  return (std::max(l1, l2) + 0.05) / (std::min(l1, l2) + 0.05);
}

Color ensureContrast(const Color& foreground, const Color& background, double minRatio, int preferLight) {
  if (contrastRatio(foreground, background) >= minRatio) {
    return foreground;
  }
  const Oklch source = toOklch(foreground);
  const bool lighten = preferLight != 0 ? preferLight > 0 : luminance(background) < 0.179;
  const auto at = [&](double lightness) {
    return fromOklch({.lightness = lightness, .chroma = source.chroma, .hueRadians = source.hueRadians});
  };
  const Color preferred = at(lighten ? 1.0 : 0.0);
  const double preferredRatio = contrastRatio(preferred, background);
  if (preferredRatio < minRatio) {
    const Color opposite = at(lighten ? 0.0 : 1.0);
    return contrastRatio(opposite, background) > preferredRatio ? opposite : preferred;
  }
  double low = lighten ? source.lightness : 0.0;
  double high = lighten ? 1.0 : source.lightness;
  Color best = preferred;
  for (int i = 0; i < 20; ++i) {
    const double lightness = (low + high) / 2.0;
    const Color test = at(lightness);
    const bool meets = contrastRatio(test, background) >= minRatio;
    if (meets) {
      best = test;
    }
    (meets == lighten ? high : low) = lightness;
  }
  return best;
}
