#pragma once

#include "render/core/mat3.h"

#include <array>
#include <string_view>

// One draw of a theme scene: a user fragment shader over the node rect.
struct ShaderSceneDraw {
  std::string_view source; // the author's GLSL ES 1.0 code that defines vec4 scene(vec2 uv)
  float width = 0.0F;
  float height = 0.0F;
  float surfaceWidth = 0.0F;
  float surfaceHeight = 0.0F;
  float scale = 1.0F;
  float time = 0.0F; // seconds
  bool dark = false;
  std::array<float, 3> surface{};
  std::array<float, 3> text{};
  std::array<float, 3> accent{};
  Mat3 transform = Mat3::identity();
  float opacity = 1.0F;
};
