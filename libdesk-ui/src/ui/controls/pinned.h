#pragma once

#include "render/scene/node.h"

// A node the size of its first child, with a second child (a badge) pinned to its bottom-right corner, `inset` in
// from both edges.
class Pinned final : public Node {
public:
  explicit Pinned(float inset) : m_inset(inset) {}

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
                                                .y = rect.height - badge.height - m_inset,
                                                .width = badge.width, .height = badge.height});
  }

private:
  float m_inset;
};
