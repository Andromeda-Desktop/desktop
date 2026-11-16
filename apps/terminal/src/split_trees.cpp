#include "split_trees.h"

#include <algorithm>
#include <utility>

namespace term {

  andromeda::tiling::DwindleTree* SplitTrees::sync(const SessionList& sessions, const std::vector<std::uint64_t>& split) {
    using Tree = andromeda::tiling::DwindleTree;
    using Members = std::vector<std::uint64_t>;
    const auto join = std::exchange(m_pending, std::nullopt);
    // Each tree follows the split that holds most of its sessions and drops the rest; a split has at most one tree.
    std::vector<std::pair<const Members*, Tree*>> followed;
    for (Tree& tree : m_trees) {
      const auto keys = tree.keys();
      const Members* best = nullptr;
      std::ptrdiff_t most = 0;
      for (const auto key : keys) {
        const Members* of = sessions.splitOf(key);
        if (const auto n = std::ranges::count(keys, of, [&sessions](auto k) { return sessions.splitOf(k); });
            of != nullptr && n > most) {
          best = of;
          most = n;
        }
      }
      if (best != nullptr && std::ranges::find(followed, best, &std::pair<const Members*, Tree*>::first) != followed.end()) {
        best = nullptr;
      }
      for (const auto key : keys) {
        if (best == nullptr || sessions.splitOf(key) != best) {
          tree.detach(tree.find(key));
        }
      }
      if (best != nullptr) {
        followed.emplace_back(best, &tree);
      }
    }
    std::erase_if(m_trees, [](const Tree& tree) { return tree.keys().size() < 2; });
    const Members* current = split.size() >= 2 ? sessions.splitOf(split.front()) : nullptr;
    if (current == nullptr) {
      return nullptr;
    }
    const auto inSplit = [&split](andromeda::tiling::TileKey key) { return std::ranges::find(split, key) != split.end(); };
    Tree* own = nullptr;
    for (Tree& tree : m_trees) {
      if (const auto keys = tree.keys(); sessions.splitOf(keys.front()) == current) {
        own = &tree;
      }
    }
    if (own == nullptr) {
      own = &m_trees.emplace_back();
    }
    if (own->empty()) {
      own->addRoot(join && inSplit(join->target) ? join->target : split.front());
    }
    for (std::size_t i = 0; i < split.size(); ++i) {
      if (own->find(split[i]) != nullptr) {
        continue;
      }
      if (join && join->joining == split[i] && own->find(join->target) != nullptr) {
        own->splitOn(own->find(join->target), split[i], join->edge);
        continue;
      }
      // Next to its neighbour in the split's order: the session it joined.
      andromeda::tiling::DwindleTree::Node* next = nullptr;
      for (std::size_t d = 1; next == nullptr && d < split.size(); ++d) {
        if (i >= d) {
          next = own->find(split[i - d]);
        }
        if (next == nullptr && i + d < split.size()) {
          next = own->find(split[i + d]);
        }
      }
      own->splitOn(next, split[i], 0);
    }
    return own;
  }

} // namespace term
