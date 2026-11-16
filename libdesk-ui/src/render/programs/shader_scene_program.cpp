#include "render/programs/shader_scene_program.h"

#include "core/log.h"

#include <cctype>
#include <stdexcept>

namespace {

  constexpr Logger kLog("scene");

  constexpr const char* kVertex = R"(
precision highp float;
attribute vec2 a_position;
uniform vec2 u_surface_size;
uniform mat3 u_transform;
uniform vec2 u_size;
varying vec2 v_uv;
void main() {
  vec3 pixel = u_transform * vec3(a_position, 1.0);
  vec2 n = pixel.xy / u_surface_size;
  v_uv = a_position / u_size;
  gl_Position = vec4(n.x * 2.0 - 1.0, 1.0 - n.y * 2.0, 0.0, 1.0);
}
)";

  // The author's code sees only these uniforms: no textures, so a scene cannot read the window or the screen.
  constexpr const char* kPreamble = R"(
precision highp float;
varying vec2 v_uv;
uniform float u_opacity;
uniform float scene_time;
uniform vec2 scene_size;
uniform float scene_scale;
uniform float scene_dark;
uniform vec3 scene_surface;
uniform vec3 scene_text;
uniform vec3 scene_accent;
#line 1
)";

  constexpr const char* kSuffix = R"(
void main() {
  vec4 c = clamp(scene(v_uv), 0.0, 1.0);
  gl_FragColor = vec4(c.rgb * c.a, c.a) * u_opacity;
}
)";

  constexpr const char* kFallbackFragment = R"(
precision mediump float;
uniform vec4 u_color;
void main() { gl_FragColor = u_color; }
)";

  bool definesMain(std::string_view s) {
    auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };
    for (std::size_t at = s.find("main"); at != std::string_view::npos; at = s.find("main", at + 4)) {
      std::size_t end = at + 4;
      while (end < s.size() && std::isspace(static_cast<unsigned char>(s[end])) != 0) {
        ++end;
      }
      if ((at == 0 || !ident(s[at - 1])) && end < s.size() && s[end] == '(') {
        return true;
      }
    }
    return false;
  }

  void drawQuad(GLint position, float w, float h) {
    const GLfloat quad[] = {0, 0, w, 0, 0, h, w, 0, w, h, 0, h};
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(static_cast<GLuint>(position));
    glVertexAttribPointer(static_cast<GLuint>(position), 2, GL_FLOAT, GL_FALSE, 0, quad);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(static_cast<GLuint>(position));
  }

} // namespace

ShaderSceneProgram::Entry& ShaderSceneProgram::entry(std::string_view source) {
  const std::string key(source);
  if (auto it = m_entries.find(key); it != m_entries.end()) {
    return it->second;
  }
  Entry& e = m_entries[key];
  // The author defines scene(); main() and extensions stay ours.
  const bool allowed = key.find("#extension") == std::string::npos && !definesMain(key);
  if (!allowed) {
    kLog.warn("scene rejected: it must not define main or use #extension");
    return e;
  }
  try {
    e.program.create(kVertex, (std::string(kPreamble) + key + kSuffix).c_str());
  } catch (const std::runtime_error& error) {
    kLog.warn("{}", error.what());
    return e;
  }
  const GLuint id = e.program.id();
  e.position = glGetAttribLocation(id, "a_position");
  e.surfaceSize = glGetUniformLocation(id, "u_surface_size");
  e.transform = glGetUniformLocation(id, "u_transform");
  e.size = glGetUniformLocation(id, "u_size");
  e.opacity = glGetUniformLocation(id, "u_opacity");
  e.time = glGetUniformLocation(id, "scene_time");
  e.sceneSize = glGetUniformLocation(id, "scene_size");
  e.scale = glGetUniformLocation(id, "scene_scale");
  e.dark = glGetUniformLocation(id, "scene_dark");
  e.surface = glGetUniformLocation(id, "scene_surface");
  e.text = glGetUniformLocation(id, "scene_text");
  e.accent = glGetUniformLocation(id, "scene_accent");
  return e;
}

void ShaderSceneProgram::draw(const ShaderSceneDraw& s) {
  Entry& e = entry(s.source);
  if (!e.program.isValid()) {
    if (!m_fallback.isValid()) {
      m_fallback.create(kVertex, kFallbackFragment);
      m_fallbackPosition = glGetAttribLocation(m_fallback.id(), "a_position");
      m_fallbackSurfaceSize = glGetUniformLocation(m_fallback.id(), "u_surface_size");
      m_fallbackTransform = glGetUniformLocation(m_fallback.id(), "u_transform");
      m_fallbackColor = glGetUniformLocation(m_fallback.id(), "u_color");
    }
    glUseProgram(m_fallback.id());
    glUniform2f(m_fallbackSurfaceSize, s.surfaceWidth, s.surfaceHeight);
    glUniformMatrix3fv(m_fallbackTransform, 1, GL_FALSE, s.transform.m.data());
    glUniform4f(m_fallbackColor, s.surface[0] * s.opacity, s.surface[1] * s.opacity, s.surface[2] * s.opacity,
                s.opacity);
    drawQuad(m_fallbackPosition, s.width, s.height);
    return;
  }
  glUseProgram(e.program.id());
  glUniform2f(e.surfaceSize, s.surfaceWidth, s.surfaceHeight);
  glUniformMatrix3fv(e.transform, 1, GL_FALSE, s.transform.m.data());
  glUniform2f(e.size, s.width, s.height);
  glUniform1f(e.opacity, s.opacity);
  glUniform1f(e.time, s.time);
  glUniform2f(e.sceneSize, s.width, s.height);
  glUniform1f(e.scale, s.scale);
  glUniform1f(e.dark, s.dark ? 1.0F : 0.0F);
  glUniform3f(e.surface, s.surface[0], s.surface[1], s.surface[2]);
  glUniform3f(e.text, s.text[0], s.text[1], s.text[2]);
  glUniform3f(e.accent, s.accent[0], s.accent[1], s.accent[2]);
  drawQuad(e.position, s.width, s.height);
}

void ShaderSceneProgram::destroy() {
  m_entries.clear();
  m_fallback.destroy();
}

void ShaderSceneProgram::abandon() noexcept {
  for (auto& [source, e] : m_entries) {
    e.program.abandon();
  }
  m_entries.clear();
  m_fallback.abandon();
}
