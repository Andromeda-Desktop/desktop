#pragma once

#include "render/core/color.h"
#include "render/scene/node.h"

class GlyphNode : public Node {
public:
  GlyphNode() : Node(NodeType::Glyph) {}

  [[nodiscard]] char32_t codepoint() const noexcept { return m_codepoint; }
  [[nodiscard]] float fontSize() const noexcept { return m_fontSize; }
  [[nodiscard]] const Color& color() const noexcept { return m_color; }

  void setCodepoint(char32_t codepoint) {
    if (m_codepoint == codepoint) {
      return;
    }
    m_codepoint = codepoint;
    markLayoutDirty();
  }

  void setFontSize(float size) {
    if (m_fontSize == size) {
      return;
    }
    m_fontSize = size;
    markLayoutDirty();
  }

  void setColor(const Color& color) {
    if (m_color == color) {
      return;
    }
    m_color = color;
    markPaintDirty();
  }

  [[nodiscard]] bool hasShadow() const noexcept { return m_hasShadow; }
  [[nodiscard]] const Color& shadowColor() const noexcept { return m_shadowColor; }
  [[nodiscard]] float shadowOffsetX() const noexcept { return m_shadowOffsetX; }
  [[nodiscard]] float shadowOffsetY() const noexcept { return m_shadowOffsetY; }
  // Blur radius of the shadow in logical px; 0 = a sharp copy.
  [[nodiscard]] float shadowBlur() const noexcept { return m_shadowBlur; }

  void setShadow(const Color& color, float offsetX, float offsetY, float blur = 0.0F) {
    m_hasShadow = true;
    m_shadowColor = color;
    m_shadowOffsetX = offsetX;
    m_shadowOffsetY = offsetY;
    m_shadowBlur = blur;
    markPaintDirty();
  }

  void clearShadow() {
    if (!m_hasShadow) {
      return;
    }
    m_hasShadow = false;
    markPaintDirty();
  }

private:
  char32_t m_codepoint = 0;
  float m_fontSize = 16.0F;
  Color m_color;
  bool m_hasShadow = false;
  Color m_shadowColor;
  float m_shadowOffsetX = 0.0F;
  float m_shadowOffsetY = 0.0F;
  float m_shadowBlur = 0.0F;
};
