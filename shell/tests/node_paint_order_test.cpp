#include "render/scene/node.h"
#include "tests/test_check.h"

#include <algorithm>
#include <memory>
#include <random>
#include <vector>

// Model test: random child edits (add, insert, remove, z-index change, z-index change of a detached node). After every
// step the cached paint order equals a stable sort of the children by z-index, and a hit test returns the topmost.
namespace {

  void runSequence(std::uint32_t seed) {
    std::mt19937 rng(seed);
    Node root;
    root.setSize(10.0F, 10.0F);
    std::vector<std::unique_ptr<Node>> detached;
    for (int step = 0; step < 200; ++step) {
      const auto& children = root.children();
      switch (rng() % 5) {
      case 0:
      case 1: {
        auto child = std::make_unique<Node>();
        child->setSize(10.0F, 10.0F);
        child->setZIndex(static_cast<std::int32_t>(rng() % 4) - 1);
        if (rng() % 2 == 0) {
          root.addChild(std::move(child));
        } else {
          root.insertChildAt(rng() % (children.size() + 1), std::move(child));
        }
        break;
      }
      case 2:
        if (!children.empty()) {
          detached.push_back(root.removeChild(children[rng() % children.size()].get()));
        }
        break;
      case 3:
        if (!children.empty()) {
          children[rng() % children.size()]->setZIndex(static_cast<std::int32_t>(rng() % 4) - 1);
        }
        break;
      default:
        if (!detached.empty()) {
          detached[rng() % detached.size()]->setZIndex(static_cast<std::int32_t>(rng() % 4) - 1);
        }
        break;
      }
      std::vector<Node*> expected;
      for (const auto& child : root.children()) {
        expected.push_back(child.get());
      }
      std::ranges::stable_sort(expected, {}, &Node::zIndex);
      TEST_CHECK(root.childrenInPaintOrder() == expected);
      Node* const top = expected.empty() ? &root : expected.back();
      TEST_CHECK(Node::hitTest(&root, 5.0F, 5.0F) == top);
    }
  }

} // namespace

int main() {
  for (std::uint32_t seed = 1; seed <= 300; ++seed) {
    runSequence(seed);
  }
  return 0;
}
