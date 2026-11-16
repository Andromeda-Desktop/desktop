#pragma once

#include <cstdint>
#include <memory>
#include <vector>

// The dwindle tiling tree, shared by the compositor's dwindle layout and app split views (terminal panes). Pure
// geometry: a tile is an opaque key, and a box is in the caller's pixel space.
namespace andromeda::tiling {

  using TileKey = std::uintptr_t; // 0 = no tile

  struct TileBox {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
  };

  // Same bit values as WLR_EDGE_*, so the compositor passes its edge masks unchanged.
  enum TileEdge : std::uint32_t { EdgeTop = 1, EdgeBottom = 2, EdgeLeft = 4, EdgeRight = 8 };

  struct Tile {
    TileKey key = 0;
    TileBox box;
  };

  class DwindleTree {
  public:
    struct Node {
      // AutoSplit is a split whose axis follows its area on arrange unless preserveSplit or an explicit directional
      // drop locks it. A tree can be built before any arrange, so the axis cannot be decided at insertion time.
      enum Type : std::uint8_t { Leaf, AutoSplit, HSplit, VSplit };
      Type type = Leaf;
      std::unique_ptr<Node> left;
      std::unique_ptr<Node> right;
      Node* parent = nullptr;
      double ratio = 0.5;
      TileKey key = 0;
      bool locked = false;
      TileBox area; // from the last arrange
    };

    // One split on the path from a leaf to the root. `first` is true when the leaf is in its left/top child;
    // outerProduct is the product of the shares of the splits above it.
    struct Split {
      Node* node = nullptr;
      bool first = false;
      double outerProduct = 1.0;
    };

    [[nodiscard]] Node* root() const { return m_root.get(); }
    void setRoot(std::unique_ptr<Node> root) { m_root = std::move(root); }
    [[nodiscard]] bool empty() const { return m_root == nullptr; }
    [[nodiscard]] Node* find(TileKey key) const;
    // The leaf at a position in left-to-right, top-to-bottom tree order.
    [[nodiscard]] Node* leafAt(int index) const;
    [[nodiscard]] std::vector<TileKey> keys() const;

    // The first tile: the root leaf. No effect on a tree that has one.
    void addRoot(TileKey key);
    // Turns `leaf` into a split of `type` with the old tile and `key`; `newFirst` puts `key` left/top.
    void splitLeaf(Node* leaf, TileKey key, Node::Type type, bool newFirst);
    // splitLeaf as an AutoSplit, with `key` on the half of an arranged leaf facing (cx, cy).
    void splitLeafToward(Node* leaf, TileKey key, double cx, double cy);
    // Splits `leaf` with `key` on `edge` (a TileEdge) and locks the axis; edge 0 is an AutoSplit, `key` last.
    void splitOn(Node* leaf, TileKey key, std::uint32_t edge);
    // Removes the node; its sibling takes the parent's place.
    void detach(Node* node);
    // Flips the split holding `leaf` between side by side and stacked, and locks it. False for a lone tile.
    bool toggleSplit(Node* leaf);

    // Lays the tree out over `area` with `gap` px between tiles and returns the leaf boxes in tree order. Unlocked
    // splits follow the longer edge unless preserveSplit is set; an AutoSplit always resolves on its first arrange.
    std::vector<Tile> arrange(const TileBox& area, int gap, bool preserveSplit);

    [[nodiscard]] std::vector<Split> splits(Node* node, Node::Type type) const;
    [[nodiscard]] static double splitShare(const Split& split);
    static void setSplitShare(const Split& split, double share);
    // Sets the shares along `splits` so the leaf takes `fraction` of the axis. False when there is no split.
    static bool applyFraction(const std::vector<Split>& splits, double fraction);

    // The split whose boundary lies behind `edge` of `leaf`: LEFT/RIGHT ride a horizontal split, TOP/BOTTOM a
    // vertical one. Null for an edge that faces the outside of the tree.
    [[nodiscard]] Node* boundary(const Node* leaf, std::uint32_t edge) const;
    // The tiles in the group a focus move from `source` to `target` enters: the subtree under the split it crossed.
    [[nodiscard]] std::vector<TileKey> peers(const Node* source, const Node* target) const;

  private:
    void arrangeNode(Node* node, const TileBox& area, int gap, bool preserveSplit, std::vector<Tile>& out);

    std::unique_ptr<Node> m_root;
  };

} // namespace andromeda::tiling
