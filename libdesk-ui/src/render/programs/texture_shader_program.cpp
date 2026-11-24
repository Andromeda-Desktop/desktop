#include "render/programs/texture_shader_program.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace {

  constexpr Logger kLog("texture-shader");

  constexpr const char* kVertex = R"(
precision highp float;
attribute vec2 a_position;
uniform vec2 u_surface_size;
uniform vec2 u_quad_size;
uniform mat3 u_transform;
varying vec2 v_texcoord;
void main() {
  v_texcoord = a_position;
  vec3 pixel = u_transform * vec3(a_position * u_quad_size, 1.0);
  vec2 n = pixel.xy / u_surface_size;
  gl_Position = vec4(n.x * 2.0 - 1.0, 1.0 - n.y * 2.0, 0.0, 1.0);
}
)";

} // namespace

TextureShaderProgram::Entry& TextureShaderProgram::entry(std::string_view fragment) {
  const std::string key(fragment);
  if (auto it = m_entries.find(key); it != m_entries.end()) {
    return it->second;
  }
  Entry& e = m_entries[key];
  try {
    e.program.create(kVertex, key.c_str());
  } catch (const std::runtime_error& error) {
    kLog.warn("{}", error.what());
    return e;
  }
  const GLuint id = e.program.id();
  e.position = glGetAttribLocation(id, "a_position");
  e.surfaceSize = glGetUniformLocation(id, "u_surface_size");
  e.quadSize = glGetUniformLocation(id, "u_quad_size");
  e.transform = glGetUniformLocation(id, "u_transform");
  e.texture = glGetUniformLocation(id, "u_texture");
  e.opacity = glGetUniformLocation(id, "u_opacity");
  return e;
}

void TextureShaderProgram::draw(const TextureShaderDraw& d) {
  if (d.texture == 0 || d.surfaceWidth <= 0.0F || d.surfaceHeight <= 0.0F || d.quadWidth <= 0.0F
      || d.quadHeight <= 0.0F) {
    return;
  }
  Entry& e = entry(d.fragment);
  if (!e.program.isValid() || e.position < 0) {
    return;
  }
  static constexpr std::array<GLfloat, 12> kQuad = {0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 1};
  const GLuint id = e.program.id();
  glUseProgram(id);
  glUniform2f(e.surfaceSize, d.surfaceWidth, d.surfaceHeight);
  glUniform2f(e.quadSize, d.quadWidth, d.quadHeight);
  glUniformMatrix3fv(e.transform, 1, GL_FALSE, d.transform.m.data());
  glUniform1f(e.opacity, std::clamp(d.opacity, 0.0F, 1.0F));
  for (const ShaderUniform& u : d.uniforms) {
    auto it = std::ranges::find(e.uniforms, u.name, &std::pair<std::string, GLint>::first);
    if (it == e.uniforms.end()) {
      it = e.uniforms.insert(e.uniforms.end(), {std::string(u.name), glGetUniformLocation(id, std::string(u.name).c_str())});
    }
    glUniform1f(it->second, u.value);
  }
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(d.texture.value()));
  glUniform1i(e.texture, 0);
  const auto position = static_cast<GLuint>(e.position);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, 0, kQuad.data());
  glEnableVertexAttribArray(position);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(position);
}

void TextureShaderProgram::destroy() { m_entries.clear(); }

void TextureShaderProgram::abandon() noexcept {
  for (auto& [source, e] : m_entries) {
    e.program.abandon();
  }
  m_entries.clear();
}
