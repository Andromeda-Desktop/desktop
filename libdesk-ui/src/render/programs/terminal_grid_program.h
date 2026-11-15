#pragma once

#include "render/core/shader_program.h"
#include "render/core/terminal_grid_draw.h"

#include <GLES2/gl2.h>

#include <span>
#include <vector>

class TerminalGridProgram {
public:
  void ensureInitialized();
  void destroy();
  void abandon() noexcept;
  void draw(const TerminalGridDraw& grid) const;

private:
  void drawFill(const TerminalGridDraw& grid) const;
  void drawRects(std::span<const std::vector<float>> lists, const TerminalGridDraw& grid) const;
  void drawGlyphs(std::span<const std::vector<float>> lists, const TerminalGridDraw& grid) const;

  mutable std::vector<float> m_fill;
  ShaderProgram m_rect;
  GLint m_rectPosition = -1;
  GLint m_rectColor = -1;
  GLint m_rectSurface = -1;
  GLint m_rectTransform = -1;
  GLint m_rectOpacity = -1;

  ShaderProgram m_glyph;
  GLint m_glyphPosition = -1;
  GLint m_glyphTexcoord = -1;
  GLint m_glyphColor = -1;
  GLint m_glyphSurface = -1;
  GLint m_glyphTransform = -1;
  GLint m_glyphOpacity = -1;
  GLint m_glyphAtlasSize = -1;
  GLint m_glyphSampler = -1;
};
