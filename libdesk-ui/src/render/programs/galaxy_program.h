#pragma once

#include "render/core/galaxy_draw.h"
#include "render/core/shader_program.h"

#include <GLES2/gl2.h>
#include <cstdint>
#include <vector>

// The galaxy: a point pass into two cached half-float layers (the far sky, the galaxy with its dust coverage), then
// a composite pass that turns and breathes the galaxy layer and tone-maps both into the target.
class GalaxyProgram {
public:
  // What the context offers for the cache; set once before the first draw.
  struct Formats {
    bool es3 = false;
    bool colorBufferHalfFloat = false;  // EXT_color_buffer_half_float
    bool colorBufferFloat = false;      // EXT_color_buffer_float (ES 3)
    bool textureHalfFloat = false;      // OES_texture_half_float (ES 2)
    bool textureHalfFloatLinear = false;
  };
  void setFormats(const Formats& formats);
  [[nodiscard]] bool hasFormats() const noexcept { return m_formatsSet; }

  // Expects premultiplied-alpha blending on entry and leaves it so.
  void draw(const GalaxyDraw& draw);
  void destroy();
  void abandon() noexcept;

private:
  struct Buffers {
    std::uint64_t id = 0;
    GLuint light = 0;
    GLuint dust = 0;
    GLuint sky = 0;
    GLuint field = 0;
    GLsizei lightCount = 0;
    GLsizei dustCount = 0;
    GLsizei skyCount = 0;
    GLsizei fieldCount = 0;
  };
  struct Layers {
    const void* key = nullptr;
    int width = 0;
    int height = 0;
    GLuint fbo[2] = {0, 0};  // 0 the far sky, 1 the galaxy (alpha = dust coverage)
    GLuint tex[2] = {0, 0};
    std::uint64_t pointsId = 0;
    GalaxyCamera camera;
    float dim = -1.0F;
    bool sparse = false;  // drawn at the moving camera's half density
    std::uint64_t lastUse = 0;
  };

  bool ensurePrograms();
  void ensureBuffers(const GalaxyPoints& points);
  Layers* layersFor(const void* key, int width, int height);
  bool createLayers(Layers& layers);
  void renderLayers(Layers& layers, const GalaxyDraw& draw);
  void drawPoints(GLuint buffer, GLsizei count, float alpha, int step);
  void releaseLayers(Layers& layers);
  void releaseBuffers();

  Formats m_formats;
  bool m_formatsSet = false;
  bool m_hdr = false;  // the layers are half-float; else RGBA8 with m_gain headroom
  float m_gain = 1.0F;

  ShaderProgram m_points;
  ShaderProgram m_field;
  ShaderProgram m_composite;
  bool m_programsTried = false;
  struct {
    GLint p = -1, c = -1, s = -1, m = -1, k = -1, dpr = -1, alpha = -1, gain = -1;
  } m_pu;
  struct {
    GLint p = -1, c = -1, m = -1, eye = -1, size = -1, alpha = -1, k = -1, dpr = -1, gain = -1;
  } m_fu;
  struct {
    GLint position = -1, surfaceSize = -1, quadSize = -1, transform = -1, bg = -1, galaxy = -1, opacity = -1, glow = -1,
          gain = -1, centre = -1, inverse = -1, light = -1, bottom = -1, top = -1, ink = -1, aurora = -1, quadPx = -1,
          scrimColor = -1, scrim = -1;
  } m_cu;

  Buffers m_buffers;
  std::vector<Layers> m_layers;
  std::uint64_t m_frame = 0;
};
