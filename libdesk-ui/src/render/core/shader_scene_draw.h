#pragma once

#include "render/core/mat3.h"

#include <array>
#include <string_view>

// What the app reports to a scene, each 0..1: busy rises while a command runs; the others pulse once (0 -> 1 -> 0).
struct SceneSignals {
  float busy = 0.0F;
  float success = 0.0F;
  float failure = 0.0F;
  float bell = 0.0F;
  bool operator==(const SceneSignals&) const = default;
};

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
  SceneSignals signals;
  Mat3 transform = Mat3::identity();
  float opacity = 1.0F;
};
