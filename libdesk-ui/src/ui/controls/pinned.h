#pragma once

#include "render/scene/node.h"

#include <cstdint>

// A node the size of its first child, with a second child (a badge) pinned to its bottom-right (or top-right) corner,
// `inset` in from both edges.
class Pinned final : public Node {
public:
  enum class Corner : std::uint8_t { BottomRight, TopRight };
  explicit Pinned(float inset, Corner corner = Corner::BottomRight) : m_inset(inset), m_corner(corner) {}

protected:
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override {
    return children().empty() ? LayoutSize{} : children().front()->measure(renderer, constraints);
  }
  void doArrange(Renderer& renderer, const LayoutRect& rect) override {
    setPosition(rect.x, rect.y);
    setSize(rect.width, rect.height);
    if (children().size() < 2) {
      return;
    }
    children()[0]->arrange(renderer, LayoutRect{.x = 0.0F, .y = 0.0F, .width = rect.width, .height = rect.height});
    const LayoutSize badge = children()[1]->measure(renderer, LayoutConstraints{});
    children()[1]->arrange(renderer, LayoutRect{.x = rect.width - badge.width - m_inset,
                                                .y = m_corner == Corner::TopRight ? m_inset : rect.height - badge.height - m_inset,
                                                .width = badge.width, .height = badge.height});
  }

private:
  float m_inset;
  Corner m_corner;
};
