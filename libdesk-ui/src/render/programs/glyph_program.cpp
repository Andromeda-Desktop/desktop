#include "render/programs/glyph_program.h"

#include "render/core/color.h"
#include "render/core/texture_handle.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

namespace {

  // Positions a unit quad, applies a pixel-space transform, converts to NDC.
  constexpr char kVertexShaderSource[] = R"(
precision highp float;

attribute vec2 a_position;
attribute vec2 a_texcoord;
uniform vec2 u_surface_size;
uniform vec2 u_size;
uniform mat3 u_transform;
varying vec2 v_texcoord;

vec2 to_ndc(vec2 pixel_pos) {
    vec2 normalized = pixel_pos / u_surface_size;
    return vec2(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0);
}

void main() {
    vec2 local = a_position * u_size;
    vec3 pixel = u_transform * vec3(local, 1.0);
    v_texcoord = a_texcoord;
    gl_Position = vec4(to_ndc(pixel.xy), 0.0, 1.0);
}
)";

  // Fragment shader: two modes selected by u_tint_mode.
  //   mode 0 (RGBA): texture stores a premultiplied RGBA glyph; scale by opacity.
  //   mode 1 (tint): texture stores an alpha coverage mask; the output is
  //                  premul(tint) * coverage * opacity.
  // Both paths output premultiplied to match the pipeline-wide
  // GL_ONE / GL_ONE_MINUS_SRC_ALPHA blend mode.
  constexpr char kFragmentShaderSource[] = R"(
precision highp float;

uniform sampler2D u_texture;
uniform float u_opacity;
uniform vec4 u_tint;        // straight (non-premul) rgba
uniform float u_tint_mode;  // 0 = RGBA texture, 1 = alpha coverage + u_tint
varying vec2 v_texcoord;

void main() {
    vec4 c = texture2D(u_texture, v_texcoord);
    if (u_tint_mode > 0.5) {
        float coverage = c.a * u_tint.a * u_opacity;
        gl_FragColor = vec4(u_tint.rgb * coverage, coverage);
    } else {
        gl_FragColor = vec4(c.rgb * u_opacity, c.a * u_opacity);
    }
}
)";

  // Alpha-coverage quads of many labels: NDC position, texcoord and premultiplied colour per vertex.
  constexpr char kBatchVertexShaderSource[] = R"(
precision highp float;

attribute vec2 a_position;
attribute vec2 a_texcoord;
attribute vec4 a_color;
varying vec2 v_texcoord;
varying vec4 v_color;

void main() {
    v_texcoord = a_texcoord;
    v_color = a_color;
    gl_Position = vec4(a_position, 0.0, 1.0);
}
)";

  constexpr char kBatchFragmentShaderSource[] = R"(
precision highp float;

uniform sampler2D u_texture;
varying vec2 v_texcoord;
varying vec4 v_color;

void main() {
    gl_FragColor = v_color * texture2D(u_texture, v_texcoord).a;
}
)";

} // namespace

void GlyphProgram::ensureInitialized() {
  if (m_program.isValid()) {
    return;
  }

  m_program.create(kVertexShaderSource, kFragmentShaderSource);
  m_positionLocation = glGetAttribLocation(m_program.id(), "a_position");
  m_texCoordLocation = glGetAttribLocation(m_program.id(), "a_texcoord");
  m_surfaceSizeLocation = glGetUniformLocation(m_program.id(), "u_surface_size");
  m_rectLocation = glGetUniformLocation(m_program.id(), "u_size");
  m_opacityLocation = glGetUniformLocation(m_program.id(), "u_opacity");
  m_samplerLocation = glGetUniformLocation(m_program.id(), "u_texture");
  m_transformLocation = glGetUniformLocation(m_program.id(), "u_transform");
  m_tintLocation = glGetUniformLocation(m_program.id(), "u_tint");
  m_tintModeLocation = glGetUniformLocation(m_program.id(), "u_tint_mode");

  if (m_positionLocation < 0
      || m_texCoordLocation < 0
      || m_surfaceSizeLocation < 0
      || m_rectLocation < 0
      || m_opacityLocation < 0
      || m_samplerLocation < 0
      || m_transformLocation < 0
      || m_tintLocation < 0
      || m_tintModeLocation < 0) {
    throw std::runtime_error("failed to query color glyph shader locations");
  }
}

void GlyphProgram::destroy() {
  m_program.destroy();
  m_positionLocation = -1;
  m_texCoordLocation = -1;
  m_surfaceSizeLocation = -1;
  m_rectLocation = -1;
  m_opacityLocation = -1;
  m_samplerLocation = -1;
  m_transformLocation = -1;
  m_tintLocation = -1;
  m_tintModeLocation = -1;
  m_batchProgram.destroy();
  m_batchPositionLocation = -1;
  m_batchTexCoordLocation = -1;
  m_batchColorLocation = -1;
  m_batchSamplerLocation = -1;
}

void GlyphProgram::abandon() noexcept {
  m_program.abandon();
  m_batchProgram.abandon();
}

void GlyphProgram::bindCommon(
    TextureId texture, float surfaceWidth, float surfaceHeight, float width, float height, float u0, float v0, float u1,
    float v1, float opacity, const Mat3& transform
) const {
  const std::array<GLfloat, 12> positions = {
      0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F, 1.0F,
  };

  const std::array<GLfloat, 12> texcoords = {
      u0, v0, u1, v0, u0, v1, u0, v1, u1, v0, u1, v1,
  };

  glUniform2f(m_surfaceSizeLocation, surfaceWidth, surfaceHeight);
  glUniform2f(m_rectLocation, width, height);
  glUniform1f(m_opacityLocation, opacity);
  glUniformMatrix3fv(m_transformLocation, 1, GL_FALSE, transform.m.data());
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture.value()));
  glUniform1i(m_samplerLocation, 0);
  const auto posAttr = static_cast<GLuint>(m_positionLocation);
  const auto texAttr = static_cast<GLuint>(m_texCoordLocation);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, positions.data());
  glVertexAttribPointer(texAttr, 2, GL_FLOAT, GL_FALSE, 0, texcoords.data());
  glEnableVertexAttribArray(posAttr);
  glEnableVertexAttribArray(texAttr);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(posAttr);
  glDisableVertexAttribArray(texAttr);
}

void GlyphProgram::draw(
    TextureId texture, float surfaceWidth, float surfaceHeight, float width, float height, float u0, float v0, float u1,
    float v1, float opacity, const Mat3& transform
) const {
  if (!m_program.isValid() || texture == 0 || width <= 0.0F || height <= 0.0F) {
    return;
  }
  glUseProgram(m_program.id());
  glUniform1f(m_tintModeLocation, 0.0F);
  glUniform4f(m_tintLocation, 1.0F, 1.0F, 1.0F, 1.0F);
  bindCommon(texture, surfaceWidth, surfaceHeight, width, height, u0, v0, u1, v1, opacity, transform);
}

void GlyphProgram::drawTinted(
    TextureId texture, float surfaceWidth, float surfaceHeight, float width, float height, float u0, float v0, float u1,
    float v1, float opacity, const Color& tint, const Mat3& transform
) const {
  if (!m_program.isValid() || texture == 0 || width <= 0.0F || height <= 0.0F) {
    return;
  }
  glUseProgram(m_program.id());
  glUniform1f(m_tintModeLocation, 1.0F);
  glUniform4f(m_tintLocation, tint.r, tint.g, tint.b, tint.a);
  bindCommon(texture, surfaceWidth, surfaceHeight, width, height, u0, v0, u1, v1, opacity, transform);
}

void GlyphProgram::ensureBatchInitialized() {
  if (m_batchProgram.isValid()) {
    return;
  }
  m_batchProgram.create(kBatchVertexShaderSource, kBatchFragmentShaderSource);
  m_batchPositionLocation = glGetAttribLocation(m_batchProgram.id(), "a_position");
  m_batchTexCoordLocation = glGetAttribLocation(m_batchProgram.id(), "a_texcoord");
  m_batchColorLocation = glGetAttribLocation(m_batchProgram.id(), "a_color");
  m_batchSamplerLocation = glGetUniformLocation(m_batchProgram.id(), "u_texture");
  if (m_batchPositionLocation < 0 || m_batchTexCoordLocation < 0 || m_batchColorLocation < 0
      || m_batchSamplerLocation < 0) {
    throw std::runtime_error("failed to query glyph batch shader locations");
  }
}

void GlyphProgram::appendTintedRun(
    std::vector<float>& out, float surfaceWidth, float surfaceHeight, std::span<const float> positions,
    std::span<const float> texcoords, float opacity, const Color& tint
) {
  const float a = tint.a * opacity;
  const float r = tint.r * a;
  const float g = tint.g * a;
  const float b = tint.b * a;
  const std::size_t vertices = std::min(positions.size(), texcoords.size()) / 2;
  const std::size_t start = out.size();
  out.resize(start + vertices * kBatchFloatsPerVertex);
  const float sx = 2.0F / surfaceWidth;
  const float sy = 2.0F / surfaceHeight;
  float* v = out.data() + start;
  for (std::size_t i = 0; i < vertices * 2; i += 2, v += kBatchFloatsPerVertex) {
    v[0] = positions[i] * sx - 1.0F;
    v[1] = 1.0F - positions[i + 1] * sy;
    v[2] = texcoords[i];
    v[3] = texcoords[i + 1];
    v[4] = r;
    v[5] = g;
    v[6] = b;
    v[7] = a;
  }
}

void GlyphProgram::drawTintedBatch(TextureId texture, const std::vector<float>& vertices) const {
  if (!m_batchProgram.isValid() || texture == 0 || vertices.empty()) {
    return;
  }
  glUseProgram(m_batchProgram.id());
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture.value()));
  glUniform1i(m_batchSamplerLocation, 0);
  constexpr auto stride = static_cast<GLsizei>(kBatchFloatsPerVertex * sizeof(float));
  const auto posAttr = static_cast<GLuint>(m_batchPositionLocation);
  const auto texAttr = static_cast<GLuint>(m_batchTexCoordLocation);
  const auto colorAttr = static_cast<GLuint>(m_batchColorLocation);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, stride, vertices.data());
  glVertexAttribPointer(texAttr, 2, GL_FLOAT, GL_FALSE, stride, vertices.data() + 2);
  glVertexAttribPointer(colorAttr, 4, GL_FLOAT, GL_FALSE, stride, vertices.data() + 4);
  glEnableVertexAttribArray(posAttr);
  glEnableVertexAttribArray(texAttr);
  glEnableVertexAttribArray(colorAttr);
  glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size() / kBatchFloatsPerVertex));
  glDisableVertexAttribArray(posAttr);
  glDisableVertexAttribArray(texAttr);
  glDisableVertexAttribArray(colorAttr);
}
