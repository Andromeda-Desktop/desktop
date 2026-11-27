#pragma once

#include <cstdint>
#include <vector>

// The phone's galaxy (Link Home): a density-wave spiral after beltoforion/Galaxy-Renderer, the same model as the
// Android GalaxyModel.kt and the desktop mockup galaxy.js. Units: galaxy radius = 1.
struct GalaxyShape {
  std::uint32_t seed = 7;
  double core = 0.31;       // bulge radius, share of the galaxy radius
  double ex1 = 0.85;        // orbit eccentricity at the core edge
  double ex2 = 0.95;        // orbit eccentricity at the rim
  double dA = 5.2;          // orbit tilt per unit radius (rad): the arms' wind
  int pertN = 2;
  double pertAmp = 40.0;
  double temp = 3000.0;     // glow colour temperature at the centre (K)
  int stars = 90000;
  double glow = 0.5;        // glow sprites per star
  int h2 = 700;             // arm-crest knots (H-alpha, young clusters, dust lanes)
  int glob = 80;            // globular clusters
};

// Interleaved points, kStride floats each: x y z, r g b a, size. size > 0 is a star (world size); size < 0 a soft
// glow sprite of world size -size.
struct GalaxyPoints {
  static constexpr int kStride = 8;
  std::uint64_t id = 0;      // unique per build: GPU buffers are cached by it
  std::vector<float> light;  // the galaxy's stars and glow, drawn additively
  std::vector<float> dust;   // the galaxy's dust lanes, drawn as coverage that dims what is under it
  std::vector<float> sky;    // far stars and nebula haze on a sphere of radius 80-95 around the galaxy
  std::vector<float> field;  // one unit cube of field stars, tiled around the eye at four scales (size unused)
};

// Deterministic for a shape: the same shape gives the same points on every platform. Takes tens of ms for 90 000
// stars: call it off the UI thread.
[[nodiscard]] GalaxyPoints buildGalaxy(const GalaxyShape& shape);
