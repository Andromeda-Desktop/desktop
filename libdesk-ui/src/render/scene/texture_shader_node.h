#pragma once

#include "render/core/texture_shader_draw.h"
#include "render/scene/node.h"

#include <algorithm>
#include <vector>

// A trusted caller's fragment shader over one texture (contract: TextureShaderDraw). Draws nothing without a texture.
class TextureShaderNode final : public Node {
public:
  TextureShaderNode() : Node(NodeType::TextureShader) { setHitTestVisible(false); }

  // The source must outlive the node (a string literal); the backend compiles each source once.
  void setFragment(std::string_view fragment) {
    if (fragment != m_fragment) {
      m_fragment = fragment;
      markPaintDirty();
    }
  }
  void setTexture(TextureId texture) {
    if (texture != m_texture) {
      m_texture = texture;
      markPaintDirty();
    }
  }
  void setUniform(std::string_view name, float value) {
    const auto it = std::ranges::find(m_uniforms, name, &ShaderUniform::name);
    if (it == m_uniforms.end()) {
      m_uniforms.push_back({.name = name, .value = value});
    } else if (it->value == value) {
      return;
    } else {
      it->value = value;
    }
    markPaintDirty();
  }

  [[nodiscard]] std::string_view fragment() const noexcept { return m_fragment; }
  [[nodiscard]] TextureId texture() const noexcept { return m_texture; }
  [[nodiscard]] std::span<const ShaderUniform> uniforms() const noexcept { return m_uniforms; }

private:
  std::string_view m_fragment;
  TextureId m_texture;
  std::vector<ShaderUniform> m_uniforms;
};
