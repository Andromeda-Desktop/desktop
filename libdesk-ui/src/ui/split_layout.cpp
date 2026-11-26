#include "ui/split_layout.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ui {

  andromeda::tiling::DwindleTree* SplitTrees::sync(const SplitOf& splitOf, const std::vector<std::uint64_t>& split, const Room& room) {
    using Tree = andromeda::tiling::DwindleTree;
    using Members = std::vector<std::uint64_t>;
    const auto join = std::exchange(m_pending, std::nullopt);
    // A tree is not pruned while no split shows, so a joining item can still hold a tile from a split it left.
    if (join) {
      for (Tree& tree : m_trees) {
        if (auto* stale = tree.find(join->joining); stale != nullptr) {
          tree.detach(stale);
        }
      }
    }
    // Each tree follows the split that holds most of its sessions and drops the rest; a split has at most one tree.
    std::vector<std::pair<const Members*, Tree*>> followed;
    for (Tree& tree : m_trees) {
      const auto keys = tree.keys();
      const Members* best = nullptr;
      std::ptrdiff_t most = 0;
      for (const auto key : keys) {
        const Members* of = splitOf(key);
        if (const auto n = std::ranges::count(keys, of, [&splitOf](auto k) { return splitOf(k); });
            of != nullptr && n > most) {
          best = of;
          most = n;
        }
      }
      if (best != nullptr && std::ranges::find(followed, best, &std::pair<const Members*, Tree*>::first) != followed.end()) {
        best = nullptr;
      }
      for (const auto key : keys) {
        if (best == nullptr || splitOf(key) != best) {
          tree.detach(tree.find(key));
        }
      }
      if (best != nullptr) {
        followed.emplace_back(best, &tree);
      }
    }
    std::erase_if(m_trees, [](const Tree& tree) { return tree.keys().size() < 2; });
    const Members* current = split.size() >= 2 ? splitOf(split.front()) : nullptr;
    if (current == nullptr) {
      return nullptr;
    }
    const auto inSplit = [&split](andromeda::tiling::TileKey key) { return std::ranges::find(split, key) != split.end(); };
    Tree* own = nullptr;
    for (Tree& tree : m_trees) {
      if (const auto keys = tree.keys(); splitOf(keys.front()) == current) {
        own = &tree;
      }
    }
    if (own == nullptr) {
      own = &m_trees.emplace_back();
    }
    if (own->empty()) {
      own->addRoot(join && inSplit(join->target) ? join->target : split.front());
    }
    // The side a session joins on follows the areas as they are now, not before a session left.
    own->arrange(room.area, room.gap, /*preserveSplit=*/true);
    for (std::size_t i = 0; i < split.size(); ++i) {
      if (own->find(split[i]) != nullptr) {
        continue;
      }
      const bool joined = join && join->joining == split[i] && own->find(join->target) != nullptr;
      if (joined && join->edge != 0) {
        own->splitOn(own->find(join->target), split[i], join->edge);
        continue;
      }
      // Next to the session it joined, else its neighbour in the split's order.
      andromeda::tiling::DwindleTree::Node* next = joined ? own->find(join->target) : nullptr;
      for (std::size_t d = 1; next == nullptr && d < split.size(); ++d) {
        if (i >= d) {
          next = own->find(split[i - d]);
        }
        if (next == nullptr && i + d < split.size()) {
          next = own->find(split[i + d]);
        }
      }
      // A leaf not arranged yet is the whole area (the first split).
      const andromeda::tiling::TileBox& at = next->area.width > 0 ? next->area : room.area;
      const bool beside = static_cast<float>(at.width - room.gap) / 2.0F >= room.minWidth;
      own->splitOn(next, split[i], beside ? andromeda::tiling::EdgeRight : andromeda::tiling::EdgeBottom);
    }
    return own;
  }


  const andromeda::tiling::DwindleTree* SplitTrees::treeOf(std::uint64_t key) const {
    const auto it = std::ranges::find_if(m_trees, [key](const auto& tree) { return tree.find(key) != nullptr; });
    return it != m_trees.end() ? &*it : nullptr;
  }

  void SplitTrees::swap(std::uint64_t a, std::uint64_t b) {
    for (andromeda::tiling::DwindleTree& tree : m_trees) {
      auto* first = tree.find(a);
      auto* second = tree.find(b);
      if (first != nullptr && second != nullptr) {
        std::swap(first->key, second->key);
        return;
      }
    }
  }

  bool SplitTrees::rotate(std::uint64_t id) {
    for (andromeda::tiling::DwindleTree& tree : m_trees) {
      if (auto* leaf = tree.find(id); leaf != nullptr) {
        return tree.toggleSplit(leaf);
      }
    }
    return false;
  }

  std::vector<PaneRect> arrangePanes(andromeda::tiling::DwindleTree& tree, const std::vector<std::uint64_t>& panes,
                                     const andromeda::tiling::TileBox& area, int gap) {
    std::vector<PaneRect> out(panes.size());
    for (const andromeda::tiling::Tile& tile : tree.arrange(area, gap, /*preserveSplit=*/true)) {
      if (const auto it = std::ranges::find(panes, tile.key); it != panes.end()) {
        out[static_cast<std::size_t>(it - panes.begin())] = {static_cast<float>(tile.box.x), static_cast<float>(tile.box.y),
                                                              static_cast<float>(tile.box.width),
                                                              static_cast<float>(tile.box.height)};
      }
    }
    return out;
  }

  std::optional<SplitDrop> splitDrop(const SplitPanes& panes, float x, float y) {
    for (const auto& [id, r] : panes) {
      if (x < r.x || y < r.y || x >= r.x + r.width || y >= r.y + r.height) {
        continue;
      }
      const float fx = (x - r.x) / r.width;
      const float fy = (y - r.y) / r.height;
      const float edge = std::min({fx, 1.0F - fx, fy, 1.0F - fy});
      const float halfW = std::floor(r.width / 2.0F);
      const float halfH = std::floor(r.height / 2.0F);
      if (edge == fx) {
        return SplitDrop{id, true, andromeda::tiling::EdgeLeft, {r.x, r.y, halfW, r.height}};
      }
      if (edge == 1.0F - fx) {
        return SplitDrop{id, false, andromeda::tiling::EdgeRight, {r.x + halfW, r.y, r.width - halfW, r.height}};
      }
      if (edge == fy) {
        return SplitDrop{id, true, andromeda::tiling::EdgeTop, {r.x, r.y, r.width, halfH}};
      }
      return SplitDrop{id, false, andromeda::tiling::EdgeBottom, {r.x, r.y + halfH, r.width, r.height - halfH}};
    }
    return std::nullopt;
  }

  std::optional<std::uint64_t> paneInDirection(const SplitPanes& panes, std::uint64_t from, float dx, float dy) {
    const auto start = std::ranges::find(panes, from, &SplitPanes::value_type::first);
    if (start == panes.end()) {
      return std::nullopt;
    }
    const auto centre = [](const PaneRect& r) { return std::pair{r.x + r.width / 2.0F, r.y + r.height / 2.0F}; };
    const auto [fx, fy] = centre(start->second);
    std::optional<std::uint64_t> best;
    float bestScore = 0.0F;
    for (const auto& [id, rect] : panes) {
      const auto [cx, cy] = centre(rect);
      const float along = (cx - fx) * dx + (cy - fy) * dy;
      if (id == from || along <= 0.0F) {
        continue;
      }
      // The nearest pane ahead, with sideways offset weighted, so Right from a stacked pane picks its row.
      const float score = along + 2.0F * std::abs(dx != 0.0F ? cy - fy : cx - fx);
      if (!best || score < bestScore) {
        best = id;
        bestScore = score;
      }
    }
    return best;
  }

  PaneRect dividerRect(const andromeda::tiling::DwindleTree::Node& split, bool vertical, float gap, float hit) {
    const andromeda::tiling::TileBox& a = split.area;
    const andromeda::tiling::TileBox& first = split.left->area;
    if (vertical) {
      return {static_cast<float>(first.x + first.width) + (gap - hit) / 2.0F, static_cast<float>(a.y), hit,
              static_cast<float>(a.height)};
    }
    return {static_cast<float>(a.x), static_cast<float>(first.y + first.height) + (gap - hit) / 2.0F,
            static_cast<float>(a.width), hit};
  }

  double dividerRatio(const andromeda::tiling::DwindleTree::Node& split, bool vertical, float at, float gap, float minWidth,
                      float minHeight) {
    using TileNode = andromeda::tiling::DwindleTree::Node;
    const float span = static_cast<float>(vertical ? split.area.width : split.area.height);
    if (span <= 0.0F) {
      return split.ratio;
    }
    // Each side keeps room for its smallest panes: side by side they add up (with the gaps), stacked the largest wins.
    const auto least = [&](auto&& self, const TileNode* node) -> float {
      if (node->type == TileNode::Leaf) {
        return vertical ? minWidth : minHeight;
      }
      const float a = self(self, node->left.get());
      const float b = self(self, node->right.get());
      return node->type == (vertical ? TileNode::HSplit : TileNode::VSplit) ? a + gap + b : std::max(a, b);
    };
    // arrange puts the gap's centre at ratio x span.
    const float lo = (least(least, split.left.get()) + gap / 2.0F) / span;
    const float hi = 1.0F - (least(least, split.right.get()) + gap / 2.0F) / span;
    return lo <= hi ? std::clamp(at / span, lo, hi) : 0.5;
  }

} // namespace ui
