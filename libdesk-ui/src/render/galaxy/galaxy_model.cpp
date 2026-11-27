// Galaxy model after beltoforion/Galaxy-Renderer.
// Copyright (c) 2012 Ingo Berg. BSD-2-Clause:
// Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
// following conditions are met: 1. Redistributions of source code must retain the above copyright notice, this list
// of conditions and the following disclaimer. 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided
// with the distribution. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
// PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
// INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "render/galaxy/galaxy_model.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

namespace {

constexpr double kTau = 6.2832;  // the mockup's rounded 2 pi: keeps the star placement identical
constexpr double kPi = 3.14159265358979323846;
constexpr double kFar = 2.0;      // radius where the orbits turn circular again
constexpr double kParsec = 13000.0;  // pc per galaxy radius

using Rgb = std::array<double, 3>;

// mulberry32, the mockup's generator: identical sequences keep the desktop, the phone and the mockup alike.
class Rng {
public:
  explicit Rng(std::uint32_t seed) : m_s(seed) {}
  double next() {
    m_s += 0x6D2B79F5u;
    std::uint32_t t = (m_s ^ (m_s >> 15)) * (1u | m_s);
    t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
    return static_cast<double>(t ^ (t >> 14)) / 4294967296.0;
  }
  // Box-Muller, one draw for the radius, then one for the angle.
  double gauss() {
    const double r = std::sqrt(-2.0 * std::log(1.0 - next()));
    return r * std::cos(kTau * next());
  }

private:
  std::uint32_t m_s;
};

Rgb kelvin(double kelvin) {
  const double t = kelvin / 100.0;
  double r = 0;
  double g = 0;
  double b = 0;
  if (t <= 66) {
    r = 255;
    g = 99.47 * std::log(t) - 161.12;
    b = t <= 19 ? 0 : 138.52 * std::log(t - 10) - 305.04;
  } else {
    r = 329.7 * std::pow(t - 60, -0.1332);
    g = 288.12 * std::pow(t - 60, -0.0755);
    b = 255;
  }
  const auto c = [](double v) { return std::clamp(v, 0.0, 255.0) / 255.0; };
  return {c(r), c(g), c(b)};
}

void put(std::vector<float>& out, double x, double y, double z, const Rgb& c, double a, double size) {
  out.insert(out.end(), {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(c[0]),
                         static_cast<float>(c[1]), static_cast<float>(c[2]), static_cast<float>(a),
                         static_cast<float>(size)});
}

struct Disc {
  const GalaxyShape& s;

  double ecc(double r) const {
    if (r < s.core) return 1 + r / s.core * (s.ex1 - 1);
    if (r <= 1) return s.ex1 + (r - s.core) / (1 - s.core) * (s.ex2 - s.ex1);
    if (r < kFar) return s.ex2 + (r - 1) / (kFar - 1) * (1 - s.ex2);
    return 1;
  }
  // A point on the orbit of semi-major axis a at angle th; the orbit tilts by a x dA unless tilt is given.
  std::array<double, 2> pos(double a, double th) const { return pos(a, th, a * s.dA); }
  std::array<double, 2> pos(double a, double th, double tilt) const {
    const double b = a * ecc(a);
    const double cb = std::cos(-tilt);
    const double sb = std::sin(-tilt);
    const double ca = std::cos(th);
    const double sa = std::sin(th);
    double x = a * ca * cb - b * sa * sb;
    double z = a * ca * sb + b * sa * cb;
    if (s.pertN != 0) {
      x += a / s.pertAmp * std::sin(th * 2 * s.pertN);
      z += a / s.pertAmp * std::cos(th * 2 * s.pertN);
    }
    return {x, z};
  }
  static double thick(double r) { return 0.012 + 0.07 * std::exp(-std::pow(r / 0.2, 2)); }
  static double jitter(double a) { return 0.06 * std::exp(-std::pow(a / 0.12, 2)); }
};

// Radial CDF: de Vaucouleurs bulge inside the core, exponential disc outside.
class Radius {
public:
  explicit Radius(double core) {
    const auto bulge = [](double x) { return std::exp(-0.02 * std::pow(x * kParsec, 0.25)); };
    const auto intensity = [&](double x) { return x < core ? bulge(x) : bulge(core) * std::exp(-(x - core) * 3); };
    m_cdf[0] = 0;
    for (int i = 1; i <= kN; ++i) m_cdf[i] = m_cdf[i - 1] + intensity(i * kFar / kN);
  }
  double draw(Rng& rnd) const {
    const double u = rnd.next() * m_cdf[kN];
    int lo = 0;
    int hi = kN;
    while (hi - lo > 1) {
      const int m = (lo + hi) >> 1;
      if (m_cdf[m] < u) lo = m;
      else hi = m;
    }
    return hi * kFar / kN;
  }

private:
  static constexpr int kN = 1000;
  std::array<double, kN + 1> m_cdf{};
};

void buildDisc(const GalaxyShape& s, GalaxyPoints& out) {
  Rng rnd(s.seed);
  const Disc disc{s};
  const Radius radius(s.core);
  auto& L = out.light;
  auto& D = out.dust;
  const auto lit = put;

  for (int i = 0; i < s.stars; ++i) {
    const double a = radius.draw(rnd);
    const auto p = disc.pos(a, rnd.next() * kTau);
    double mag = 0.1 + 0.4 * rnd.next();
    if (i < s.stars / 60.0) mag = std::min(1.0, mag + 0.1 + 0.4 * rnd.next());
    const double j = Disc::jitter(a);
    const double x = p[0] + rnd.gauss() * j;
    const double y = rnd.gauss() * Disc::thick(a);
    const double z = p[1] + rnd.gauss() * j;
    const Rgb c = kelvin(3800 + a * 6500 + 1500 * rnd.next());
    lit(L, x, y, z, c, 0.35 + mag, 0.0022 + mag * 0.006);
  }

  // Glow (the renderer's "dust"): soft, warm inside, blue outside.
  for (int i = 0; i < s.stars * s.glow; ++i) {
    double a = 0;
    if (i % 2 != 0) {
      a = std::min(radius.draw(rnd), 1.15);
    } else {
      const double x = 2 * rnd.next() - 1;
      const double z = 2 * rnd.next() - 1;
      a = std::hypot(x, z) * 0.8;
    }
    const auto p = disc.pos(a, rnd.next() * kTau);
    const double mag = 0.02 + 0.15 * rnd.next();
    const double j = Disc::jitter(a);
    const double x = p[0] + rnd.gauss() * j;
    const double y = rnd.gauss() * Disc::thick(a) * 0.7;
    const double z = p[1] + rnd.gauss() * j;
    lit(L, x, y, z, kelvin(s.temp + a * 8000), mag * 0.14, -(mag * 0.2));
  }

  // Filaments: short arcs of glow along one orbit.
  for (int i = 0; i < s.stars * s.glow / 100; ++i) {
    const double x0 = 2 * rnd.next() - 1;
    const double z0 = 2 * rnd.next() - 1;
    double a = std::hypot(x0, z0);
    const double th = rnd.next() * kTau;
    const double mag = 0.1 + 0.05 * rnd.next();
    const int m = static_cast<int>(std::floor(100 * rnd.next()));
    for (int k = 0; k < m; ++k) {
      a += (200 - 400 * rnd.next()) / kParsec;
      const auto p = disc.pos(a, th + (10 - 20 * rnd.next()) * kPi / 180);
      const double y = rnd.gauss() * 0.006;
      const double alpha = (mag + 0.025 * rnd.next()) * 0.15;
      lit(L, p[0], y, p[1], kelvin(s.temp + a * kParsec / 4.5 - 1000), alpha, -(mag * 0.14));
    }
  }

  // Arm crests: where neighbouring orbits crowd (rho), gas ignites: H-alpha knots, young clusters, dust lanes.
  const double d = 1000 / kParsec;
  int knots = 0;
  for (int i = 0; i < s.h2 * 12 && knots < s.h2; ++i) {
    const double x0 = 2 * rnd.next() - 1;
    const double z0 = 2 * rnd.next() - 1;
    const double a = std::hypot(x0, z0);
    if (a < 0.12 || a > 1) continue;
    const double th = rnd.next() * kTau;
    const double aI = std::max(a - d, 0.0);
    const double aO = a + d;
    const double tA = a * s.dA;
    const auto p = disc.pos(a, th);
    const auto pI = disc.pos(aI, th - (tA - aI * s.dA), aI * s.dA);
    const auto pO = disc.pos(aO, th + (aO * s.dA - tA), aO * s.dA);
    const double rho = 0.5 * ((a - aI) / std::max(std::hypot(p[0] - pI[0], p[1] - pI[1]), 1 / kParsec) +
                              d / std::max(std::hypot(p[0] - pO[0], p[1] - pO[1]), 1 / kParsec));
    const double ig = std::clamp((rho - 1.25) / 0.6, 0.0, 1.0);
    if (ig <= 0) continue;
    ++knots;
    const double y = rnd.gauss() * 0.006;
    lit(L, p[0], y, p[1], {1, 0.25, 0.42}, 0.6 * ig, -(0.016 + 0.024 * rnd.next()));
    for (int k = 0; k < 12; ++k) {
      const double x = p[0] + rnd.gauss() * 0.006;
      const double yy = y + rnd.gauss() * 0.003;
      const double z = p[1] + rnd.gauss() * 0.006;
      lit(L, x, yy, z, {0.75, 0.85, 1}, 0.8, 0.0022 + rnd.next() * 0.003);
    }
    const double aq = a - d * 0.6;
    const auto q = disc.pos(aq, th - (tA - aq * s.dA), aq * s.dA);
    for (int k = 0; k < 10; ++k) {
      const double x = q[0] + rnd.gauss() * 0.02;
      const double yy = rnd.gauss() * 0.004;
      const double z = q[1] + rnd.gauss() * 0.02;
      lit(D, x, yy, z, {0, 0, 0}, 0.06, -(0.03 + rnd.next() * 0.04));
    }
  }

  for (int i = 0; i < s.glob; ++i) {
    const double r = 0.25 + rnd.next() * 1.1;
    const double u = rnd.next() * kTau;
    const double v = std::acos(2 * rnd.next() - 1);
    const double x = r * std::sin(v) * std::cos(u);
    const double y = r * std::cos(v) * 0.6;
    const double z = r * std::sin(v) * std::sin(u);
    for (int k = 0; k < 60; ++k) {
      const double g = std::abs(rnd.gauss());
      const double px = x + rnd.gauss() * 0.004 * g;
      const double py = y + rnd.gauss() * 0.004 * g;
      const double pz = z + rnd.gauss() * 0.004 * g;
      lit(L, px, py, pz, {1, 0.88, 0.72}, 0.7, 0.0018 + rnd.next() * 0.002);
    }
  }
}

void buildSky(GalaxyPoints& out) {
  Rng rnd(99);
  for (int i = 0; i < 7000; ++i) {
    const double u = rnd.next() * kTau;
    const double v = std::acos(2 * rnd.next() - 1);
    const double t = rnd.next();
    const Rgb c = t < 0.3 ? Rgb{0.7, 0.8, 1} : t < 0.8 ? Rgb{1, 0.97, 0.92} : Rgb{1, 0.8, 0.6};
    constexpr double R = 80;
    const double alpha = 0.35 + 0.6 * rnd.next();
    const double size = 0.035 + std::pow(rnd.next(), 4) * 0.12;
    put(out.sky, R * std::sin(v) * std::cos(u), R * std::cos(v), R * std::sin(v) * std::sin(u), c, alpha, size);
  }
  // Nebula haze on a band tilted by (0.5, 0.3) rad.
  const double ca = std::cos(0.5);
  const double sa = std::sin(0.5);
  const double cz = std::cos(0.3);
  const double sz = std::sin(0.3);
  for (int i = 0; i < 160; ++i) {
    double x = rnd.gauss();
    double y = rnd.gauss() * 0.22;
    double z = rnd.gauss();
    const double l = std::sqrt(x * x + y * y + z * z);
    constexpr double R = 95;
    x = x / l * R;
    y = y / l * R;
    z = z / l * R;
    const double y1 = y * ca - z * sa;
    const double z1 = y * sa + z * ca;
    const double t = rnd.next();
    const Rgb c = t < 0.4 ? Rgb{0.22, 0.28, 0.8} : t < 0.75 ? Rgb{0.5, 0.22, 0.7} : Rgb{0.15, 0.5, 0.6};
    put(out.sky, x * cz - y1 * sz, x * sz + y1 * cz, z1, c, 0.02, -(6 + rnd.next() * 14));
  }
}

void buildField(GalaxyPoints& out) {
  Rng rnd(5);
  for (int i = 0; i < 9000; ++i) {
    const double t = rnd.next();
    const Rgb c = t < 0.3 ? Rgb{0.7, 0.8, 1} : t < 0.8 ? Rgb{1, 0.96, 0.9} : Rgb{1, 0.78, 0.58};
    const double x = rnd.next();
    const double y = rnd.next();
    const double z = rnd.next();
    put(out.field, x, y, z, c, 0.25 + 0.75 * std::pow(rnd.next(), 2), 0);
  }
}

}  // namespace

GalaxyPoints buildGalaxy(const GalaxyShape& shape) {
  static std::atomic<std::uint64_t> next{1};
  GalaxyPoints out;
  out.id = next.fetch_add(1, std::memory_order_relaxed);
  buildDisc(shape, out);
  buildSky(out);
  buildField(out);
  return out;
}
