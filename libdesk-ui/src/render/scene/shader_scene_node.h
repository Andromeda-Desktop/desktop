#pragma once

#include "render/core/color.h"
#include "render/scene/node.h"

#include <string>

// A theme scene: the author's fragment shader over the node rect, drawn under other content.
class ShaderSceneNode : public Node {
public:
  ShaderSceneNode() : Node(NodeType::ShaderScene) {}

  void setSource(std::string source) {
    if (source != m_source) {
      m_source = std::move(source);
      markPaintDirty();
    }
  }
  void setTime(float seconds) {
    if (seconds != m_time) {
      m_time = seconds;
      markPaintDirty();
    }
  }
  // The theme anchors the scene may draw with; dark tells it which twin is in use.
  void setColors(const Color& surface, const Color& text, const Color& accent, bool dark) {
    m_surface = surface;
    m_text = text;
    m_accent = accent;
    m_dark = dark;
    markPaintDirty();
  }

  void setSignals(const SceneSignals& signals) {
    if (signals != m_signals) {
      m_signals = signals;
      markPaintDirty();
    }
  }

  [[nodiscard]] const std::string& source() const noexcept { return m_source; }
  [[nodiscard]] const SceneSignals& signals() const noexcept { return m_signals; }
  [[nodiscard]] float time() const noexcept { return m_time; }
  [[nodiscard]] const Color& surface() const noexcept { return m_surface; }
  [[nodiscard]] const Color& text() const noexcept { return m_text; }
  [[nodiscard]] const Color& accent() const noexcept { return m_accent; }
  [[nodiscard]] bool dark() const noexcept { return m_dark; }

private:
  std::string m_source;
  float m_time = 0.0F;
  SceneSignals m_signals;
  Color m_surface;
  Color m_text;
  Color m_accent;
  bool m_dark = false;
};
