#pragma once

#include "render/core/shader_program.h"
#include "render/core/texture_shader_draw.h"

#include <GLES2/gl2.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Compiles each texture-shader source once. A source that fails to compile draws nothing.
class TextureShaderProgram {
public:
  void destroy();
  void abandon() noexcept;
  void draw(const TextureShaderDraw& draw);

private:
  struct Entry {
    ShaderProgram program; // invalid when the source failed to compile
    GLint position = -1;
    GLint surfaceSize = -1;
    GLint quadSize = -1;
    GLint transform = -1;
    GLint texture = -1;
    GLint opacity = -1;
    std::vector<std::pair<std::string, GLint>> uniforms;
  };

  Entry& entry(std::string_view fragment);

  std::unordered_map<std::string, Entry> m_entries;
};
