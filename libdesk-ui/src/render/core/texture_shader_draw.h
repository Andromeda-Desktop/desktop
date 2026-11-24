#pragma once

#include "render/core/mat3.h"
#include "render/core/texture_handle.h"

#include <span>
#include <string_view>

// One float uniform of a texture shader; the name must outlive every draw (a string literal).
struct ShaderUniform {
  std::string_view name;
  float value = 0.0F;
  bool operator==(const ShaderUniform&) const = default;
};

// One draw of a trusted caller's GLSL ES 1.0 fragment shader over the node rect. The shader defines main() and may
// read: varying vec2 v_texcoord (0..1 over the rect), uniform sampler2D u_texture, uniform float u_opacity, and the
// named uniforms. It writes premultiplied colour.
struct TextureShaderDraw {
  std::string_view fragment;
  TextureId texture;
  std::span<const ShaderUniform> uniforms;
  float surfaceWidth = 0.0F;
  float surfaceHeight = 0.0F;
  float quadWidth = 0.0F;
  float quadHeight = 0.0F;
  float opacity = 1.0F;
  Mat3 transform = Mat3::identity();
};
