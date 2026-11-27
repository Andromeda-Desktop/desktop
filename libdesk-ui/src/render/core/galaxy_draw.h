#pragma once

#include "render/core/mat3.h"
#include "render/galaxy/galaxy_model.h"
#include "render/galaxy/galaxy_view.h"

#include <memory>
#include <optional>

// One draw of a galaxy over the node rect. The backend renders the points into a cache keyed by `key` and redraws
// the points only when points, camera, dim, moving or the rect's pixel size change; life and the sky only re-run
// the composite pass.
struct GalaxyDraw {
  const void* key = nullptr;  // GalaxyNode::cacheOwner(); one cache per owner
  std::shared_ptr<const GalaxyPoints> points;
  GalaxyCamera camera;
  float dim = 1.0F;  // alpha of the galaxy's own points (the far sky stays)
  std::optional<GalaxyLightSky> lightSky;  // empty = the dark sky
  GalaxyLife life;
  bool moving = false;  // the camera changes each frame (drag, zoom, morph); the still after it redraws in full
  float surfaceWidth = 0.0F;
  float surfaceHeight = 0.0F;
  float quadWidth = 0.0F;
  float quadHeight = 0.0F;
  float scale = 1.0F;  // buffer px per logical px
  float opacity = 1.0F;
  Mat3 transform = Mat3::identity();
};
