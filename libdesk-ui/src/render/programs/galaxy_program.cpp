#include "render/programs/galaxy_program.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace {

  constexpr Logger kLog("galaxy");

  constexpr GLenum kRgba16f = 0x881A;      // GL_RGBA16F (ES 3)
  constexpr GLenum kHalfFloat = 0x140B;    // GL_HALF_FLOAT (ES 3)
  constexpr GLenum kHalfFloatOes = 0x8D61; // GL_HALF_FLOAT_OES (OES_texture_half_float)
  // While the camera moves, the layers render at this share of the resolution (a quarter of the fill); the still after
  // it renders in full.
  constexpr float movingResolution(bool moving) { return moving ? 0.5F : 1.0F; }
  // RGBA8 fallback: light is stored at this share so the core can add up past 1 before it clips.
  constexpr float kLdrGain = 0.25F;
  constexpr std::size_t kMaxLayers = 2;
  constexpr float kFov = 40.0F * std::numbers::pi_v<float> / 180.0F;
  constexpr GLsizei kStrideBytes = GalaxyPoints::kStride * sizeof(float);

  // Stars (s > 0) shrink to a 1.3 px floor and fade instead; glow sprites (s < 0) fade past 140 px.
  constexpr const char* kPointVertex = R"(
attribute vec3 p;
attribute vec4 c;
attribute float s;
uniform mat4 M;
uniform float K, dpr, A;
varying vec4 v;
varying float h;
void main() {
  vec4 q = M * vec4(p, 1.0);
  gl_Position = q;
  float d = max(q.w, 1e-4), a = c.a * A, z;
  if (s > 0.0) {
    z = s * K / d; float m = 1.3 * dpr;
    if (z < m) { a *= z * z / (m * m); z = m; }
    z = min(z, 7.0 * dpr); h = 1.0;
  } else {
    z = -s * K / d; float b = 140.0 * dpr;
    if (z > b) { a *= b * b * b / (z * z * z); }
    z = min(z, 600.0);
    if (z < 1.0) { a *= z; z = 1.0; }
    h = 0.0;
  }
  if (q.w <= 0.0) a = 0.0;
  gl_PointSize = z;
  v = vec4(c.rgb, a);
}
)";

  // One unit cube of field stars tiled around the eye at scale S.
  constexpr const char* kFieldVertex = R"(
attribute vec3 p;
attribute vec4 c;
uniform mat4 M;
uniform vec3 E;
uniform float S, A, K, dpr;
varying vec4 v;
varying float h;
void main() {
  vec3 w = E + mod(p * S - E + S * 0.5, S) - S * 0.5;
  vec4 q = M * vec4(w, 1.0);
  gl_Position = q;
  float d = max(q.w, 1e-6);
  float a = c.a * A * (1.0 - smoothstep(0.55, 1.0, length(w - E) / (S * 0.5)));
  float z = c.a * 0.004 * S * K / d, m = 1.3 * dpr;
  if (z < m) { a *= z * z / (m * m); z = m; }
  z = min(z, 5.0 * dpr);
  if (q.w <= 0.0) a = 0.0;
  gl_PointSize = z;
  v = vec4(c.rgb, a);
  h = 1.0;
}
)";

  constexpr const char* kPointFragment = R"(
precision mediump float;
uniform float G;
varying vec4 v;
varying float h;
void main() {
  vec2 d = gl_PointCoord - 0.5;
  float r2 = dot(d, d) * 4.0;
  float f = h > 0.5 ? exp(-r2 * 7.0) : exp(-r2 * 3.5);
  f *= step(r2, 1.0);
  gl_FragColor = vec4(v.rgb * v.a * f * G, v.a * f);
}
)";

  constexpr const char* kCompositeVertex = R"(
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

  // HDR light -> 1 - exp(-light), saturated. Dark: on black. Light: inks the day sky in each star's own hue (white
  // stars in indigo), then the aurora curtains.
  constexpr const char* kCompositeFragment = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D u_bg;
uniform sampler2D u_galaxy;
uniform float u_opacity, u_glow, u_gain, u_light, u_ink, u_aurora;
uniform vec2 u_quad_px, u_centre;
uniform mat2 u_inverse;
uniform vec3 u_bottom, u_top;
uniform vec3 u_scrim_color;
uniform vec4 u_scrim;  // alpha at x 0, alpha at mid, mid x, end x (logical px)
uniform vec4 u_ring;        // centre x, centre y (logical px), phase, y/x ratio
uniform vec3 u_ring_size;   // start x radius, end x radius, stroke width (logical px)
uniform vec4 u_ring_color;  // rgb, alpha at the start (0 = no rings)
varying vec2 v_texcoord;
const vec3 LUM = vec3(0.3, 0.59, 0.11);
void main() {
  vec2 t = vec2(v_texcoord.x, 1.0 - v_texcoord.y);
  vec2 px = v_texcoord * u_quad_px;
  vec2 src = (u_centre + u_inverse * (px - u_centre)) / u_quad_px;
  vec4 g = texture2D(u_galaxy, vec2(src.x, 1.0 - src.y));
  if (src.x < 0.0 || src.x > 1.0 || src.y < 0.0 || src.y > 1.0) g = vec4(0.0);
  vec3 bg = texture2D(u_bg, t).rgb;
  vec3 h = (bg * (1.0 - g.a) + g.rgb * u_glow) * u_gain;
  vec3 c;
  if (u_light < 0.5) {
    c = 1.0 - exp(-h * 1.5);
    c = max(mix(vec3(dot(c, LUM)), c, 1.5), 0.0);
  } else {
    vec3 s = 1.0 - exp(-h * 1.2);
    s = max(mix(vec3(dot(s, LUM)), s, 1.4), 0.0);
    vec3 day = mix(u_bottom, u_top, smoothstep(0.2, 1.0, t.y));
    float gy = dot(s, LUM);
    vec3 hu = s / max(max(s.r, max(s.g, s.b)), 1e-3);
    float sa = max(hu.r, max(hu.g, hu.b)) - min(hu.r, min(hu.g, hu.b));
    hu = clamp(mix(vec3(dot(hu, LUM)), hu, 1.8), 0.0, 1.0);
    hu = mix(vec3(0.3, 0.36, 0.68), hu, smoothstep(0.06, 0.4, sa)) * 0.8;
    day = mix(day, hu, clamp(pow(gy, 0.75) * u_ink * 1.7, 0.0, 0.94));
    for (int k = 0; k < 2; k++) {
      float f = float(k);
      float yc = 0.6 + f * 0.17 + 0.06 * sin(t.x * (5.3 - f * 2.0) + 1.0 + f * 2.0) + 0.025 * sin(t.x * 13.1 + f);
      float d = t.y - yc;
      float fall = d > 0.0 ? exp(-d * (7.0 + f * 5.0)) : exp(-d * d * 1100.0);
      float st = 0.8 + 0.2 * sin(t.x * (23.0 + f * 11.0) + sin(t.x * 4.0 + f) * 3.0);
      vec3 col = mix(vec3(0.22, 0.78, 0.62), vec3(0.58, 0.46, 0.92), clamp(d * 4.0 + f * 0.6, 0.0, 1.0));
      day = mix(day, col, clamp(fall * st * u_aurora * (0.6 - f * 0.2), 0.0, 0.7));
    }
    c = day;
  }
  float a = px.x < u_scrim.z ? mix(u_scrim.x, u_scrim.y, px.x / u_scrim.z)
                             : u_scrim.y * (1.0 - clamp((px.x - u_scrim.z) / (u_scrim.w - u_scrim.z), 0.0, 1.0));
  vec3 o = mix(pow(c, vec3(0.9)), u_scrim_color, a);
  if (u_ring_color.a > 0.0) {
    vec2 q = vec2(px.x - u_ring.x, (px.y - u_ring.y) / u_ring.w);
    float lq = max(length(q), 1e-3);
    // |grad| of length(q) in px turns the radial gap into a distance across the stroke.
    float gl = max(length(vec2(q.x, q.y / u_ring.w)) / lq, 1e-3);
    for (int k = 0; k < 3; k++) {
      float u = fract(u_ring.z + float(k) / 3.0);
      float d = abs(lq - mix(u_ring_size.x, u_ring_size.y, u)) / gl;
      o = mix(o, u_ring_color.rgb, clamp(u_ring_size.z * 0.5 + 0.5 - d, 0.0, 1.0) * (1.0 - u) * u_ring_color.a);
    }
  }
  gl_FragColor = vec4(o, 1.0) * u_opacity;
}
)";

  GLuint upload(const std::vector<float>& data) {
    GLuint id = 0;
    glGenBuffers(1, &id);
    glBindBuffer(GL_ARRAY_BUFFER, id);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)), data.data(), GL_STATIC_DRAW);
    return id;
  }

  GLsizei count(const std::vector<float>& data) { return static_cast<GLsizei>(data.size() / GalaxyPoints::kStride); }

} // namespace

void GalaxyProgram::setFormats(const Formats& formats) {
  m_formats = formats;
  m_formatsSet = true;
}

bool GalaxyProgram::ensurePrograms() {
  if (m_programsTried) {
    return m_composite.isValid();
  }
  m_programsTried = true;
  try {
    m_points.create(kPointVertex, kPointFragment);
    m_field.create(kFieldVertex, kPointFragment);
    m_composite.create(kCompositeVertex, kCompositeFragment);
  } catch (const std::runtime_error& error) {
    kLog.warn("{}", error.what());
    m_points.destroy();
    m_field.destroy();
    m_composite.destroy();
    return false;
  }
  GLuint id = m_points.id();
  m_pu = {glGetAttribLocation(id, "p"), glGetAttribLocation(id, "c"), glGetAttribLocation(id, "s"),
          glGetUniformLocation(id, "M"), glGetUniformLocation(id, "K"), glGetUniformLocation(id, "dpr"),
          glGetUniformLocation(id, "A"), glGetUniformLocation(id, "G")};
  id = m_field.id();
  m_fu = {glGetAttribLocation(id, "p"), glGetAttribLocation(id, "c"), glGetUniformLocation(id, "M"),
          glGetUniformLocation(id, "E"), glGetUniformLocation(id, "S"), glGetUniformLocation(id, "A"),
          glGetUniformLocation(id, "K"), glGetUniformLocation(id, "dpr"), glGetUniformLocation(id, "G")};
  id = m_composite.id();
  m_cu.position = glGetAttribLocation(id, "a_position");
  m_cu.surfaceSize = glGetUniformLocation(id, "u_surface_size");
  m_cu.quadSize = glGetUniformLocation(id, "u_quad_size");
  m_cu.transform = glGetUniformLocation(id, "u_transform");
  m_cu.bg = glGetUniformLocation(id, "u_bg");
  m_cu.galaxy = glGetUniformLocation(id, "u_galaxy");
  m_cu.opacity = glGetUniformLocation(id, "u_opacity");
  m_cu.glow = glGetUniformLocation(id, "u_glow");
  m_cu.gain = glGetUniformLocation(id, "u_gain");
  m_cu.centre = glGetUniformLocation(id, "u_centre");
  m_cu.inverse = glGetUniformLocation(id, "u_inverse");
  m_cu.light = glGetUniformLocation(id, "u_light");
  m_cu.bottom = glGetUniformLocation(id, "u_bottom");
  m_cu.top = glGetUniformLocation(id, "u_top");
  m_cu.ink = glGetUniformLocation(id, "u_ink");
  m_cu.aurora = glGetUniformLocation(id, "u_aurora");
  m_cu.quadPx = glGetUniformLocation(id, "u_quad_px");
  m_cu.scrimColor = glGetUniformLocation(id, "u_scrim_color");
  m_cu.scrim = glGetUniformLocation(id, "u_scrim");
  m_cu.ring = glGetUniformLocation(id, "u_ring");
  m_cu.ringSize = glGetUniformLocation(id, "u_ring_size");
  m_cu.ringColor = glGetUniformLocation(id, "u_ring_color");
  return true;
}

void GalaxyProgram::ensureBuffers(const GalaxyPoints& points) {
  if (m_buffers.id == points.id) {
    return;
  }
  releaseBuffers();
  m_buffers = {
      .id = points.id,
      .light = upload(points.light),
      .dust = upload(points.dust),
      .sky = upload(points.sky),
      .field = upload(points.field),
      .lightCount = count(points.light),
      .dustCount = count(points.dust),
      .skyCount = count(points.sky),
      .fieldCount = count(points.field),
  };
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

bool GalaxyProgram::createLayers(Layers& layers) {
  const bool es3Hdr = m_formats.es3 && (m_formats.colorBufferHalfFloat || m_formats.colorBufferFloat);
  const bool es2Hdr = !m_formats.es3 && m_formats.textureHalfFloat && m_formats.colorBufferHalfFloat;
  const bool linear = m_formats.es3 || m_formats.textureHalfFloatLinear;
  GLint previousFbo = 0;
  GLint previousTexture = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFbo);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
  const auto restore = [&] {
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFbo));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
  };
  for (int tryHdr = (es3Hdr || es2Hdr) ? 1 : 0; tryHdr >= 0; --tryHdr) {
    m_hdr = tryHdr == 1;
    m_gain = m_hdr ? 1.0F : kLdrGain;
    const GLint internal = m_hdr && m_formats.es3 ? static_cast<GLint>(kRgba16f) : GL_RGBA;
    const GLenum type = !m_hdr ? GL_UNSIGNED_BYTE : m_formats.es3 ? kHalfFloat : kHalfFloatOes;
    const GLint filter = !m_hdr || linear ? GL_LINEAR : GL_NEAREST;
    bool complete = true;
    for (int i = 0; i < 2; ++i) {
      glGenTextures(1, &layers.tex[i]);
      glBindTexture(GL_TEXTURE_2D, layers.tex[i]);
      glTexImage2D(GL_TEXTURE_2D, 0, internal, layers.width, layers.height, 0, GL_RGBA, type, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glGenFramebuffers(1, &layers.fbo[i]);
      glBindFramebuffer(GL_FRAMEBUFFER, layers.fbo[i]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, layers.tex[i], 0);
      complete = complete && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    restore();
    if (complete) {
      return true;
    }
    releaseLayers(layers);
    if (m_hdr) {
      kLog.info("half-float layers incomplete; using RGBA8");
    }
  }
  return false;
}

GalaxyProgram::Layers* GalaxyProgram::layersFor(const void* key, int width, int height) {
  auto it = std::ranges::find(m_layers, key, &Layers::key);
  if (it != m_layers.end() && (it->width != width || it->height != height)) {
    releaseLayers(*it);
    m_layers.erase(it);
    it = m_layers.end();
  }
  if (it == m_layers.end()) {
    if (m_layers.size() >= kMaxLayers) {
      auto oldest = std::ranges::min_element(m_layers, {}, &Layers::lastUse);
      releaseLayers(*oldest);
      m_layers.erase(oldest);
    }
    Layers fresh{.key = key, .width = width, .height = height};
    if (!createLayers(fresh)) {
      return nullptr;
    }
    m_layers.push_back(fresh);
    it = m_layers.end() - 1;
  }
  it->lastUse = ++m_frame;
  return &*it;
}

void GalaxyProgram::drawPoints(GLuint buffer, GLsizei n, float alpha, int step) {
  glBindBuffer(GL_ARRAY_BUFFER, buffer);
  const GLsizei stride = kStrideBytes * step;
  const auto p = static_cast<GLuint>(m_pu.p);
  const auto c = static_cast<GLuint>(m_pu.c);
  const auto s = static_cast<GLuint>(m_pu.s);
  glVertexAttribPointer(p, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
  glVertexAttribPointer(c, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(3 * sizeof(float)));
  glVertexAttribPointer(s, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(7 * sizeof(float)));
  glEnableVertexAttribArray(p);
  glEnableVertexAttribArray(c);
  glEnableVertexAttribArray(s);
  glUniform1f(m_pu.alpha, alpha * static_cast<float>(step));
  glDrawArrays(GL_POINTS, 0, n / step);
  glDisableVertexAttribArray(p);
  glDisableVertexAttribArray(c);
  glDisableVertexAttribArray(s);
}

void GalaxyProgram::renderLayers(Layers& layers, const GalaxyDraw& d) {
  GLint previousFbo = 0;
  std::array<GLint, 4> viewport{};
  std::array<GLfloat, 4> clearColor{};
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFbo);
  glGetIntegerv(GL_VIEWPORT, viewport.data());
  glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor.data());
  const bool scissor = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
  const bool stencil = glIsEnabled(GL_STENCIL_TEST) == GL_TRUE;
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_STENCIL_TEST);
  glEnable(GL_BLEND);
  glClearColor(0, 0, 0, 0);

  const auto w = static_cast<float>(layers.width);
  const auto h = static_cast<float>(layers.height);
  const GalaxyMat4 m = galaxyViewProjection(d.camera, w, h);
  const float k = h / std::tan(kFov / 2.0F) / 2.0F;
  const float dpr = std::min(d.scale, 2.0F) * movingResolution(d.moving);

  // While the camera moves, every second point at twice the alpha: the same light on average, half the fill.
  const int step = d.moving ? 2 : 1;
  const GLsizei stride = kStrideBytes * step;

  // Layer 0: the far sky and the field stars, additive.
  glBindFramebuffer(GL_FRAMEBUFFER, layers.fbo[0]);
  glViewport(0, 0, layers.width, layers.height);
  glClear(GL_COLOR_BUFFER_BIT);
  glBlendFunc(GL_ONE, GL_ONE);
  glUseProgram(m_field.id());
  glUniformMatrix4fv(m_fu.m, 1, GL_FALSE, m.data());
  const auto eye = galaxyEye(d.camera);
  glUniform3f(m_fu.eye, eye[0], eye[1], eye[2]);
  glUniform1f(m_fu.k, k);
  glUniform1f(m_fu.dpr, dpr);
  glUniform1f(m_fu.gain, m_gain);
  glBindBuffer(GL_ARRAY_BUFFER, m_buffers.field);
  const auto fp = static_cast<GLuint>(m_fu.p);
  const auto fc = static_cast<GLuint>(m_fu.c);
  glVertexAttribPointer(fp, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
  glVertexAttribPointer(fc, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(3 * sizeof(float)));
  glEnableVertexAttribArray(fp);
  glEnableVertexAttribArray(fc);
  // Four octaves, 4x apart, cross-fade with zoom so the field looks the same at every distance.
  const float octave = std::log(d.camera.dist) / std::log(4.0F);
  const float base = std::floor(octave);
  for (int j = 0; j < 4; ++j) {
    const float x = static_cast<float>(j) + 1.0F - (octave - base);
    glUniform1f(m_fu.size, std::pow(4.0F, base + static_cast<float>(j)) * 1.5F);
    glUniform1f(m_fu.alpha, std::pow(std::sin(std::numbers::pi_v<float> * x / 4.0F), 2.0F) * static_cast<float>(step));
    glDrawArrays(GL_POINTS, 0, m_buffers.fieldCount / step);
  }
  glDisableVertexAttribArray(fp);
  glDisableVertexAttribArray(fc);

  glUseProgram(m_points.id());
  glUniformMatrix4fv(m_pu.m, 1, GL_FALSE, m.data());
  glUniform1f(m_pu.k, k);
  glUniform1f(m_pu.dpr, dpr);
  glUniform1f(m_pu.gain, m_gain);
  drawPoints(m_buffers.sky, m_buffers.skyCount, 1.0F, step);

  // Layer 1: the galaxy's light (alpha kept 0), then its dust: dims the light and records coverage in alpha.
  glBindFramebuffer(GL_FRAMEBUFFER, layers.fbo[1]);
  glClear(GL_COLOR_BUFFER_BIT);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
  drawPoints(m_buffers.light, m_buffers.lightCount, d.dim, step);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  drawPoints(m_buffers.dust, m_buffers.dustCount, d.dim, step);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFbo));
  glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
  glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
  if (scissor) glEnable(GL_SCISSOR_TEST);
  if (stencil) glEnable(GL_STENCIL_TEST);
  glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

  layers.pointsId = d.points->id;
  layers.camera = d.camera;
  layers.dim = d.dim;
  layers.sparse = d.moving;
}

void GalaxyProgram::draw(const GalaxyDraw& d) {
  if (!d.points || d.quadWidth <= 0.0F || d.quadHeight <= 0.0F || d.surfaceWidth <= 0.0F || d.surfaceHeight <= 0.0F
      || !ensurePrograms()) {
    return;
  }
  const float resolution = d.scale * movingResolution(d.moving);
  const int width = std::max(1, static_cast<int>(std::lround(d.quadWidth * resolution)));
  const int height = std::max(1, static_cast<int>(std::lround(d.quadHeight * resolution)));
  ensureBuffers(*d.points);
  Layers* layers = layersFor(d.key, width, height);
  if (layers == nullptr) {
    return;
  }
  if (layers->pointsId != d.points->id || layers->camera != d.camera || layers->dim != d.dim
      || layers->sparse != d.moving) {
    renderLayers(*layers, d);
  }

  const GalaxySpin spin = galaxySpin(d.camera, d.quadWidth, d.quadHeight, d.life.angle);
  // Inverse of the 2 x 2 turn: the composite samples the cached galaxy where each pixel came from.
  const float det = spin.m[0] * spin.m[3] - spin.m[2] * spin.m[1];
  const std::array<GLfloat, 4> inverse = {spin.m[3] / det, -spin.m[1] / det, -spin.m[2] / det, spin.m[0] / det};
  static constexpr std::array<GLfloat, 12> kQuad = {0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 1};

  glUseProgram(m_composite.id());
  glUniform2f(m_cu.surfaceSize, d.surfaceWidth, d.surfaceHeight);
  glUniform2f(m_cu.quadSize, d.quadWidth, d.quadHeight);
  glUniform2f(m_cu.quadPx, d.quadWidth, d.quadHeight);
  glUniformMatrix3fv(m_cu.transform, 1, GL_FALSE, d.transform.m.data());
  glUniform1f(m_cu.opacity, std::clamp(d.opacity, 0.0F, 1.0F));
  glUniform1f(m_cu.glow, d.life.glow);
  glUniform1f(m_cu.gain, 1.0F / m_gain);
  glUniform2f(m_cu.centre, spin.centre[0], spin.centre[1]);
  glUniformMatrix2fv(m_cu.inverse, 1, GL_FALSE, inverse.data());
  const GalaxyScrim& scrim = d.scrim;
  const float strength = std::clamp(scrim.strength, 0.0F, 1.0F);
  const float midX = std::max(scrim.midX, 1.0F);
  glUniform3f(m_cu.scrimColor, scrim.color[0], scrim.color[1], scrim.color[2]);
  glUniform4f(m_cu.scrim, scrim.edge * strength, scrim.mid * strength, midX, std::max(scrim.endX, midX + 1.0F));
  const GalaxyRings& rings = d.rings;
  glUniform4f(m_cu.ring, rings.centre[0], rings.centre[1], rings.phase, std::max(rings.ratio, 0.01F));
  glUniform3f(m_cu.ringSize, rings.start, rings.end, rings.width);
  glUniform4f(m_cu.ringColor, rings.color[0], rings.color[1], rings.color[2], std::clamp(rings.alpha, 0.0F, 1.0F));
  glUniform1f(m_cu.light, d.lightSky ? 1.0F : 0.0F);
  if (d.lightSky) {
    glUniform3f(m_cu.bottom, d.lightSky->bottom[0], d.lightSky->bottom[1], d.lightSky->bottom[2]);
    glUniform3f(m_cu.top, d.lightSky->top[0], d.lightSky->top[1], d.lightSky->top[2]);
    glUniform1f(m_cu.ink, d.lightSky->ink);
    glUniform1f(m_cu.aurora, d.lightSky->aurora);
  }
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, layers->tex[1]);
  glUniform1i(m_cu.galaxy, 1);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, layers->tex[0]);
  glUniform1i(m_cu.bg, 0);
  const auto position = static_cast<GLuint>(m_cu.position);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, 0, kQuad.data());
  glEnableVertexAttribArray(position);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(position);
}

void GalaxyProgram::releaseLayers(Layers& layers) {
  glDeleteFramebuffers(2, layers.fbo);
  glDeleteTextures(2, layers.tex);
  layers.fbo[0] = layers.fbo[1] = 0;
  layers.tex[0] = layers.tex[1] = 0;
}

void GalaxyProgram::releaseBuffers() {
  const std::array<GLuint, 4> ids = {m_buffers.light, m_buffers.dust, m_buffers.sky, m_buffers.field};
  glDeleteBuffers(4, ids.data());
  m_buffers = {};
}

void GalaxyProgram::destroy() {
  for (Layers& layers : m_layers) {
    releaseLayers(layers);
  }
  m_layers.clear();
  if (m_buffers.id != 0) {
    releaseBuffers();
  }
  m_points.destroy();
  m_field.destroy();
  m_composite.destroy();
  m_programsTried = false;
}

void GalaxyProgram::abandon() noexcept {
  m_layers.clear();
  m_buffers = {};
  m_points.abandon();
  m_field.abandon();
  m_composite.abandon();
  m_programsTried = false;
}
