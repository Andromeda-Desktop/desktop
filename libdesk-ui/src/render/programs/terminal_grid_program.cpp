#include "render/programs/terminal_grid_program.h"

namespace {

  constexpr char kRectVertex[] = R"(
precision highp float;

attribute vec2 a_position;
attribute vec4 a_color;
uniform vec2 u_surface_size;
uniform mat3 u_transform;
uniform float u_opacity;
varying vec4 v_color;

void main() {
    vec3 pixel = u_transform * vec3(a_position, 1.0);
    vec2 n = pixel.xy / u_surface_size;
    v_color = a_color * u_opacity;
    gl_Position = vec4(n.x * 2.0 - 1.0, 1.0 - n.y * 2.0, 0.0, 1.0);
}
)";

  constexpr char kRectFragment[] = R"(
precision mediump float;
varying vec4 v_color;
void main() { gl_FragColor = v_color; }
)";

  constexpr char kGlyphVertex[] = R"(
precision highp float;

attribute vec2 a_position;
attribute vec2 a_texcoord;
attribute vec4 a_color;
uniform vec2 u_surface_size;
uniform mat3 u_transform;
uniform float u_opacity;
uniform vec2 u_atlas_size;
varying vec2 v_texcoord;
varying vec4 v_color;

void main() {
    vec3 pixel = u_transform * vec3(a_position, 1.0);
    vec2 n = pixel.xy / u_surface_size;
    v_texcoord = a_texcoord / u_atlas_size;
    v_color = a_color * u_opacity;
    gl_Position = vec4(n.x * 2.0 - 1.0, 1.0 - n.y * 2.0, 0.0, 1.0);
}
)";

  // The atlas is premultiplied RGBA: coverage glyphs are white and take v_color, colour glyphs get a white v_color.
  constexpr char kGlyphFragment[] = R"(
precision mediump float;
uniform sampler2D u_atlas;
varying vec2 v_texcoord;
varying vec4 v_color;
void main() { gl_FragColor = v_color * texture2D(u_atlas, v_texcoord); }
)";

  constexpr GLsizei kRectStride = 6 * sizeof(float);
  constexpr GLsizei kGlyphStride = 8 * sizeof(float);

} // namespace

void TerminalGridProgram::ensureInitialized() {
  if (m_rect.isValid() && m_glyph.isValid()) {
    return;
  }
  m_rect.create(kRectVertex, kRectFragment);
  m_rectPosition = glGetAttribLocation(m_rect.id(), "a_position");
  m_rectColor = glGetAttribLocation(m_rect.id(), "a_color");
  m_rectSurface = glGetUniformLocation(m_rect.id(), "u_surface_size");
  m_rectTransform = glGetUniformLocation(m_rect.id(), "u_transform");
  m_rectOpacity = glGetUniformLocation(m_rect.id(), "u_opacity");

  m_glyph.create(kGlyphVertex, kGlyphFragment);
  m_glyphPosition = glGetAttribLocation(m_glyph.id(), "a_position");
  m_glyphTexcoord = glGetAttribLocation(m_glyph.id(), "a_texcoord");
  m_glyphColor = glGetAttribLocation(m_glyph.id(), "a_color");
  m_glyphSurface = glGetUniformLocation(m_glyph.id(), "u_surface_size");
  m_glyphTransform = glGetUniformLocation(m_glyph.id(), "u_transform");
  m_glyphOpacity = glGetUniformLocation(m_glyph.id(), "u_opacity");
  m_glyphAtlasSize = glGetUniformLocation(m_glyph.id(), "u_atlas_size");
  m_glyphSampler = glGetUniformLocation(m_glyph.id(), "u_atlas");
}

void TerminalGridProgram::destroy() {
  m_rect.destroy();
  m_glyph.destroy();
}

void TerminalGridProgram::abandon() noexcept {
  m_rect.abandon();
  m_glyph.abandon();
}

void TerminalGridProgram::draw(const TerminalGridDraw& grid) const {
  std::vector<float>& fill = m_fill;
  fill.clear();
  const float corners[6][2] = {{0, 0}, {grid.width, 0}, {0, grid.height}, {grid.width, 0}, {grid.width, grid.height}, {0, grid.height}};
  for (const auto& c : corners) {
    fill.insert(fill.end(), {c[0], c[1], grid.background[0], grid.background[1], grid.background[2], grid.background[3]});
  }
  drawRects({&fill, 1}, grid);
  drawRects(grid.rects, grid);
  drawGlyphs(grid.glyphs, grid);
  drawRects(grid.over, grid);
  drawGlyphs(grid.overGlyphs, grid);
}

void TerminalGridProgram::drawRects(std::span<const std::vector<float>> lists, const TerminalGridDraw& grid) const {
  glUseProgram(m_rect.id());
  glUniform2f(m_rectSurface, grid.surfaceWidth, grid.surfaceHeight);
  glUniformMatrix3fv(m_rectTransform, 1, GL_FALSE, grid.transform.m.data());
  glUniform1f(m_rectOpacity, grid.opacity);
  const auto position = static_cast<GLuint>(m_rectPosition);
  const auto color = static_cast<GLuint>(m_rectColor);
  glEnableVertexAttribArray(position);
  glEnableVertexAttribArray(color);
  for (const auto& vertices : lists) {
    if (vertices.empty()) {
      continue;
    }
    glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, kRectStride, vertices.data());
    glVertexAttribPointer(color, 4, GL_FLOAT, GL_FALSE, kRectStride, vertices.data() + 2);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size() / 6));
  }
  glDisableVertexAttribArray(position);
  glDisableVertexAttribArray(color);
}

void TerminalGridProgram::drawGlyphs(std::span<const std::vector<float>> lists, const TerminalGridDraw& grid) const {
  if (!grid.atlas.valid()) {
    return;
  }
  glUseProgram(m_glyph.id());
  glUniform2f(m_glyphSurface, grid.surfaceWidth, grid.surfaceHeight);
  glUniformMatrix3fv(m_glyphTransform, 1, GL_FALSE, grid.transform.m.data());
  glUniform1f(m_glyphOpacity, grid.opacity);
  glUniform2f(m_glyphAtlasSize, static_cast<float>(grid.atlasWidth), static_cast<float>(grid.atlasHeight));
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(grid.atlas.value()));
  glUniform1i(m_glyphSampler, 0);
  const auto position = static_cast<GLuint>(m_glyphPosition);
  const auto texcoord = static_cast<GLuint>(m_glyphTexcoord);
  const auto color = static_cast<GLuint>(m_glyphColor);
  glEnableVertexAttribArray(position);
  glEnableVertexAttribArray(texcoord);
  glEnableVertexAttribArray(color);
  for (const auto& vertices : lists) {
    if (vertices.empty()) {
      continue;
    }
    glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, kGlyphStride, vertices.data());
    glVertexAttribPointer(texcoord, 2, GL_FLOAT, GL_FALSE, kGlyphStride, vertices.data() + 2);
    glVertexAttribPointer(color, 4, GL_FLOAT, GL_FALSE, kGlyphStride, vertices.data() + 4);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size() / 8));
  }
  glDisableVertexAttribArray(position);
  glDisableVertexAttribArray(texcoord);
  glDisableVertexAttribArray(color);
}
