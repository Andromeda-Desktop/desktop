#pragma once

#include <array>
#include <optional>

// An orbit camera around the galaxy's core (the origin). sx, sy shift the core on screen (NDC, -1..1) so it can sit
// beside other content.
struct GalaxyCamera {
  float yaw = 0.55F;
  float pitch = 0.42F;  // rad above the disc
  float dist = 3.0F;    // galaxy radii from the core
  float sx = 0.12F;
  float sy = 0.12F;
  bool operator==(const GalaxyCamera&) const = default;
};

// The light theme's sky: a vertical gradient with aurora curtains; the galaxy inks it in its own hues.
struct GalaxyLightSky {
  std::array<float, 3> bottom{0.955F, 0.968F, 0.975F};
  std::array<float, 3> top{0.83F, 0.87F, 0.93F};
  float ink = 0.9F;     // how strongly stars darken the sky
  float aurora = 0.8F;  // curtain strength, 0 = none
  bool operator==(const GalaxyLightSky&) const = default;
};

// A darkening from the left edge that keeps text over the sky readable: `color` at alpha `edge` at x 0, `mid` at
// `midX`, clear from `endX` (logical px of the node). Alphas scale by `strength`; 0 draws none.
struct GalaxyScrim {
  std::array<float, 3> color{0.0F, 0.0F, 0.0F};
  float edge = 0.0F;
  float mid = 0.0F;
  float midX = 1.0F;
  float endX = 2.0F;
  float strength = 0.0F;
  bool operator==(const GalaxyScrim&) const = default;
};

// While something runs on the phone the galaxy turns about its core and the core breathes. Both act on the cached
// image: angle rotates the disc in its own plane (an affine map of the image), glow scales the galaxy's light.
struct GalaxyLife {
  float angle = 0.0F;  // rad
  float glow = 1.0F;
  bool operator==(const GalaxyLife&) const = default;
};

using GalaxyMat4 = std::array<float, 16>;  // column-major

// The view-projection of the camera for a viewport of width x height (any unit; only the aspect counts).
[[nodiscard]] GalaxyMat4 galaxyViewProjection(const GalaxyCamera& camera, float width, float height);

// The eye position in galaxy units.
[[nodiscard]] std::array<float, 3> galaxyEye(const GalaxyCamera& camera);

// The life turn as an image map: p' = centre + m (p - centre), in the viewport's y-down units.
struct GalaxySpin {
  std::array<float, 2> centre{};
  std::array<float, 4> m{1, 0, 0, 1};  // column-major 2 x 2
};
[[nodiscard]] GalaxySpin galaxySpin(const GalaxyCamera& camera, float width, float height, float angle);

// Where a galaxy point lands in a width x height viewport (y down), after the life turn; empty behind the eye.
[[nodiscard]] std::optional<std::array<float, 2>>
galaxyProject(const GalaxyCamera& camera, float width, float height, const std::array<float, 3>& point, float angle);
