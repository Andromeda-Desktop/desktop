#pragma once

#include "render/core/shader_program.h"
#include "render/core/shader_scene_draw.h"

#include <GLES2/gl2.h>
#include <string>
#include <unordered_map>

// Compiles each scene source once. A source that fails to compile draws the surface colour instead.
class ShaderSceneProgram {
public:
  void destroy();
  void abandon() noexcept;
  void draw(const ShaderSceneDraw& scene);

private:
  struct Entry {
    ShaderProgram program; // invalid when the source failed to compile
    GLint position = -1;
    GLint surfaceSize = -1;
    GLint transform = -1;
    GLint size = -1;
    GLint opacity = -1;
    GLint time = -1;
    GLint sceneSize = -1;
    GLint scale = -1;
    GLint dark = -1;
    GLint surface = -1;
    GLint text = -1;
    GLint accent = -1;
  };

  Entry& entry(std::string_view source);

  std::unordered_map<std::string, Entry> m_entries;
  ShaderProgram m_fallback;
  GLint m_fallbackPosition = -1;
  GLint m_fallbackSurfaceSize = -1;
  GLint m_fallbackTransform = -1;
  GLint m_fallbackColor = -1;
};
