#include "dwindle_tree.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>

namespace andromeda::tiling {

  namespace {

    // Calls visit(leaf) for each leaf in tree order until it returns true; returns that leaf.
    template <typename Visit> DwindleTree::Node* walkLeaves(DwindleTree::Node* root, Visit&& visit) {
      std::vector<DwindleTree::Node*> stack;
      if (root != nullptr) {
        stack.push_back(root);
      }
      while (!stack.empty()) {
        DwindleTree::Node* node = stack.back();
        stack.pop_back();
        if (node->type == DwindleTree::Node::Leaf) {
          if (visit(node)) {
            return node;
          }
          continue;
        }
        if (node->right != nullptr) {
          stack.push_back(node->right.get());
        }
        if (node->left != nullptr) {
          stack.push_back(node->left.get());
        }
      }
      return nullptr;
    }

  } // namespace

  DwindleTree::Node* DwindleTree::find(TileKey key) const {
    if (key == 0) {
      return nullptr;
    }
    return walkLeaves(m_root.get(), [key](const Node* leaf) { return leaf->key == key; });
  }

  DwindleTree::Node* DwindleTree::leafAt(int index) const {
    if (index < 0) {
      return nullptr;
    }
    int count = 0;
    return walkLeaves(m_root.get(), [&count, index](const Node*) { return count++ == index; });
  }

  std::vector<TileKey> DwindleTree::keys() const {
    std::vector<TileKey> out;
    walkLeaves(m_root.get(), [&out](const Node* leaf) {
      if (leaf->key != 0) {
        out.push_back(leaf->key);
      }
      return false;
    });
    return out;
  }

  std::vector<DwindleTree::ShapeNode> DwindleTree::shape() const {
    std::vector<ShapeNode> out;
    const auto walk = [&out](const auto& self, const Node* node) -> void {
      if (node == nullptr) {
        return;
      }
      out.push_back({.type = node->type, .ratio = node->ratio, .locked = node->locked, .key = node->key});
      self(self, node->left.get());
      self(self, node->right.get());
    };
    walk(walk, m_root.get());
    return out;
  }

  bool DwindleTree::setShape(std::span<const ShapeNode> shape) {
    // Pre-order without recursion: a saved shape is outside data, so its depth has no bound. Count first, so a malformed
    // shape allocates nothing (a deep partial tree would recurse in ~Node).
    std::size_t need = 1; // subtrees still to read
    for (const ShapeNode& at : shape) {
      if (need == 0) {
        return false;
      }
      need = need - 1 + (at.type == Node::Leaf ? 0 : 2); // a node fills one place; a split opens two
    }
    if (need != 0) {
      return false;
    }
    std::unique_ptr<Node> root;
    std::vector<Node*> open; // splits still waiting for a child
    for (const ShapeNode& at : shape) {
      if (root != nullptr && open.empty()) {
        return false;
      }
      const bool leaf = at.type == Node::Leaf;
      if (leaf ? at.key == 0 : at.type > Node::VSplit || !(at.ratio > 0.0 && at.ratio < 1.0)) {
        return false;
      }
      auto node = std::make_unique<Node>();
      Node* made = node.get();
      node->type = at.type;
      node->locked = at.locked;
      node->key = leaf ? at.key : 0;
      node->ratio = leaf ? 0.5 : at.ratio;
      if (root == nullptr) {
        root = std::move(node);
      } else {
        Node* parent = open.back();
        node->parent = parent;
        if (parent->left == nullptr) {
          parent->left = std::move(node);
        } else {
          parent->right = std::move(node);
          open.pop_back();
        }
      }
      if (!leaf) {
        open.push_back(made);
      }
    }
    if (root == nullptr || !open.empty()) {
      return false;
    }
    m_root = std::move(root);
    return true;
  }

  void DwindleTree::addRoot(TileKey key) {
    if (m_root != nullptr) {
      return;
    }
    m_root = std::make_unique<Node>();
    m_root->key = key;
  }

  void DwindleTree::splitLeaf(Node* leaf, TileKey key, Node::Type type, bool newFirst) {
    if (leaf == nullptr) {
      return;
    }
    const TileKey old = leaf->key;
    leaf->key = 0;
    leaf->type = type;
    leaf->ratio = 0.5;

    auto first = std::make_unique<Node>();
    first->parent = leaf;
    auto second = std::make_unique<Node>();
    second->parent = leaf;
    first->key = newFirst ? key : old;
    second->key = newFirst ? old : key;
    leaf->left = std::move(first);
    leaf->right = std::move(second);
  }

  void DwindleTree::splitLeafToward(Node* leaf, TileKey key, double cx, double cy) {
    // Same rule arrangeNode applies when it resolves the AutoSplit below.
    const TileBox& a = leaf->area;
    const bool horizontal = a.width >= a.height;
    const bool newFirst = horizontal ? cx < a.x + (a.width / 2.0) : cy < a.y + (a.height / 2.0);
    splitLeaf(leaf, key, Node::AutoSplit, newFirst);
  }

  void DwindleTree::splitOn(Node* leaf, TileKey key, std::uint32_t edge) {
    if (leaf == nullptr || leaf->type != Node::Leaf) {
      return;
    }
    if (edge == 0) {
      splitLeaf(leaf, key, Node::AutoSplit, /*newFirst=*/false);
      return;
    }
    const bool horizontal = (edge & (EdgeLeft | EdgeRight)) != 0;
    const bool newFirst = (edge & (EdgeLeft | EdgeTop)) != 0;
    splitLeaf(leaf, key, horizontal ? Node::HSplit : Node::VSplit, newFirst);
    leaf->locked = true;
  }

  DwindleTree::Place DwindleTree::placeOf(const Node* leaf) const {
    Place place;
    const Node* parent = leaf->parent;
    if (parent == nullptr) {
      place.root = true;
      return place;
    }
    place.first = parent->left.get() == leaf;
    place.type = parent->type;
    place.ratio = parent->ratio;
    place.locked = parent->locked;
    for (const Node* node = parent; node->parent != nullptr; node = node->parent) {
      place.path.push_back(node->parent->left.get() == node);
    }
    std::ranges::reverse(place.path);
    return place;
  }

  void DwindleTree::insertAt(TileKey key, const Place& place) {
    if (m_root == nullptr) {
      addRoot(key);
      return;
    }
    std::unique_ptr<Node>* slot = &m_root;
    for (const bool left : place.path) {
      Node* node = slot->get();
      if (node->type == Node::Leaf) {
        break;
      }
      slot = left ? &node->left : &node->right;
    }
    auto split = std::make_unique<Node>();
    split->parent = (*slot)->parent;
    split->type = place.root ? Node::AutoSplit : place.type;
    split->ratio = place.ratio;
    split->locked = place.locked;
    auto leaf = std::make_unique<Node>();
    leaf->key = key;
    leaf->parent = split.get();
    (*slot)->parent = split.get();
    // A former root leaf returns first, as the tree had it.
    const bool first = place.root || place.first;
    split->left = first ? std::move(leaf) : std::move(*slot);
    split->right = first ? std::move(*slot) : std::move(leaf);
    *slot = std::move(split);
  }

  void DwindleTree::detach(Node* node) {
    if (node == nullptr) {
      return;
    }
    Node* parent = node->parent;
    if (parent == nullptr) {
      m_root.reset();
      return;
    }
    Node* grandparent = parent->parent;
    Node* sibling = (parent->left.get() == node) ? parent->right.get() : parent->left.get();
    if (sibling == nullptr) {
      if (grandparent != nullptr) {
        if (grandparent->left.get() == parent) {
          grandparent->left.reset();
        } else {
          grandparent->right.reset();
        }
      } else {
        m_root.reset();
      }
      return;
    }
    sibling->parent = nullptr;
    std::unique_ptr<Node> ownedSibling;
    if (parent->left.get() == node) {
      ownedSibling = std::move(parent->right);
    } else {
      ownedSibling = std::move(parent->left);
    }
    parent->left.reset();
    parent->right.reset();
    ownedSibling->parent = grandparent;
    if (grandparent != nullptr) {
      if (grandparent->left.get() == parent) {
        grandparent->left = std::move(ownedSibling);
      } else {
        grandparent->right = std::move(ownedSibling);
      }
    } else {
      m_root = std::move(ownedSibling);
    }
  }

  bool DwindleTree::toggleSplit(Node* leaf) {
    if (leaf == nullptr || leaf->parent == nullptr) {
      return false;
    }
    Node* parent = leaf->parent;
    const bool horizontal =
        parent->type == Node::AutoSplit ? parent->area.width >= parent->area.height : parent->type == Node::HSplit;
    parent->type = horizontal ? Node::VSplit : Node::HSplit;
    parent->locked = true;
    return true;
  }

  std::vector<Tile> DwindleTree::arrange(const TileBox& area, int gap, bool preserveSplit) {
    std::vector<Tile> out;
    arrangeNode(m_root.get(), area, gap, preserveSplit, out);
    return out;
  }

  void DwindleTree::arrangeNode(Node* node, const TileBox& area, int gap, bool preserveSplit, std::vector<Tile>& out) {
    if (node == nullptr || area.width <= 0 || area.height <= 0) {
      return;
    }
    node->area = area;
    if (node->type == Node::Leaf) {
      if (node->key != 0) {
        out.push_back({.key = node->key, .box = area});
      }
      return;
    }

    if (node->type == Node::AutoSplit || (!node->locked && !preserveSplit)) {
      // With preserveSplit, the first arrange resolves AutoSplit and then freezes it; explicit drops stay fixed.
      node->type = area.width >= area.height ? Node::HSplit : Node::VSplit;
    }

    const double ratio = std::clamp(node->ratio, 0.1, 0.9);
    TileBox first = area;
    TileBox second = area;
    if (node->type == Node::HSplit) {
      first.width = std::max(1, static_cast<int>(std::lround(ratio * area.width)) - gap / 2);
      second.x = area.x + first.width + gap;
      second.width = std::max(1, area.width - first.width - gap);
    } else {
      first.height = std::max(1, static_cast<int>(std::lround(ratio * area.height)) - gap / 2);
      second.y = area.y + first.height + gap;
      second.height = std::max(1, area.height - first.height - gap);
    }
    arrangeNode(node->left.get(), first, gap, preserveSplit, out);
    arrangeNode(node->right.get(), second, gap, preserveSplit, out);
  }

  std::vector<DwindleTree::Split> DwindleTree::splits(Node* node, Node::Type type) const {
    std::vector<Split> result;
    Node* child = node;
    while (child != nullptr && child->parent != nullptr) {
      Node* parent = child->parent;
      if (parent->type == type) {
        result.push_back({.node = parent, .first = parent->left.get() == child});
      }
      child = parent;
    }

    double outerProduct = 1.0;
    for (auto& split : std::views::reverse(result)) {
      split.outerProduct = outerProduct;
      outerProduct *= splitShare(split);
    }
    return result;
  }

  double DwindleTree::splitShare(const Split& split) { return split.first ? split.node->ratio : 1.0 - split.node->ratio; }

  void DwindleTree::setSplitShare(const Split& split, double share) {
    split.node->ratio = split.first ? share : 1.0 - share;
  }

  bool DwindleTree::applyFraction(const std::vector<Split>& splits, double fraction) {
    if (splits.empty()) {
      return false;
    }
    const double used = std::clamp(fraction, 0.1, 1.0);
    double innerProduct = 1.0;
    for (const Split& split : splits) {
      const double denominator = innerProduct * split.outerProduct;
      const double desired = denominator > 0.0 ? used / denominator : std::numeric_limits<double>::infinity();
      const double applied = std::clamp(desired, 0.1, 0.9);
      setSplitShare(split, applied);
      innerProduct *= applied;
      if (applied == desired) {
        break;
      }
    }
    return true;
  }

  DwindleTree::Node* DwindleTree::boundary(const Node* leaf, std::uint32_t edge) const {
    if (leaf == nullptr) {
      return nullptr;
    }
    // LEFT/TOP mean the leaf sits in the split's second (right/bottom) child.
    const bool wantHorizontal = (edge & (EdgeLeft | EdgeRight)) != 0;
    const bool wantSecond = (edge & (EdgeLeft | EdgeTop)) != 0;
    const Node* current = leaf;
    while (current->parent != nullptr) {
      Node* parent = current->parent;
      if ((parent->type == Node::HSplit) == wantHorizontal && (parent->right.get() == current) == wantSecond) {
        return parent;
      }
      current = parent;
    }
    return nullptr;
  }

  std::vector<TileKey> DwindleTree::peers(const Node* source, const Node* target) const {
    if (source == nullptr || target == nullptr || source == target) {
      return {};
    }
    std::vector<const Node*> sourceChain;
    for (const Node* node = source; node != nullptr; node = node->parent) {
      sourceChain.push_back(node);
    }
    // Climb from the target until its parent is the split the move crossed; that subtree is the group it entered.
    const Node* crossed = target;
    while (crossed->parent != nullptr && std::ranges::find(sourceChain, crossed->parent) == sourceChain.end()) {
      crossed = crossed->parent;
    }
    std::vector<TileKey> out;
    walkLeaves(const_cast<Node*>(crossed), [&out](const Node* leaf) {
      if (leaf->key != 0) {
        out.push_back(leaf->key);
      }
      return false;
    });
    return out;
  }

} // namespace andromeda::tiling
