#pragma once

#include "protocols/dwindle_tree.h"

#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <utility>
#include <vector>

namespace ui {

  // Split panes in an app window (Terminal sessions, Viewer documents): the compositor's dwindle tree per split and
  // the geometry the app's pane cards follow. Keys are the app's item ids.
  struct PaneRect {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
  };

  class SplitTrees {
  public:
    // The split that holds `key` (its members in order), or null.
    using SplitOf = std::function<const std::vector<std::uint64_t>*(std::uint64_t)>;
    struct Join {
      std::uint64_t target = 0;
      std::uint64_t joining = 0;
      std::uint32_t edge = 0; // a TileEdge; 0: beside or below, by room
    };
    // The next sync puts `joining` on `edge` of `target`.
    void pendJoin(const Join& join) { m_pending = join; }
    // Where a split lays out: its area (px), the gap between panes and the narrowest pane.
    struct Room {
      andromeda::tiling::TileBox area;
      int gap = 0;
      float minWidth = 0.0F;
    };
    // The tree of `split`, with the items that left it removed and the ones that joined it inserted; the other trees
    // lose items that are no longer in a split. Null for a lone item. An item that was not dropped goes beside its
    // neighbour when both keep `room.minWidth`, else below it.
    andromeda::tiling::DwindleTree* sync(const SplitOf& splitOf, const std::vector<std::uint64_t>& split, const Room& room);
    [[nodiscard]] const std::list<andromeda::tiling::DwindleTree>& trees() const { return m_trees; }

  private:
    std::list<andromeda::tiling::DwindleTree> m_trees; // a list: erasing a tree keeps the others' nodes in place
    std::optional<Join> m_pending;
  };

  using SplitPanes = std::vector<std::pair<std::uint64_t, PaneRect>>;

  // The panes' rects in `area` (preserveSplit: a pane keeps its side when the window changes shape), in `panes` order.
  [[nodiscard]] std::vector<PaneRect> arrangePanes(andromeda::tiling::DwindleTree& tree, const std::vector<std::uint64_t>& panes,
                                                   const andromeda::tiling::TileBox& area, int gap);

  // An item dragged onto a pane: the half under the point (nearest edge) and its preview rect.
  struct SplitDrop {
    std::uint64_t target = 0;
    bool before = false;    // the left or top half
    std::uint32_t edge = 0; // the TileEdge of that half
    PaneRect rect;
  };
  [[nodiscard]] std::optional<SplitDrop> splitDrop(const SplitPanes& panes, float x, float y);

  // The pane after `from` in direction (dx, dy) (one of them 0, the other +-1), or nullopt.
  [[nodiscard]] std::optional<std::uint64_t> paneInDirection(const SplitPanes& panes, std::uint64_t from, float dx, float dy);

  // The hit rect of the divider between `split`'s halves (vertical: between columns), over the gap, along the split.
  [[nodiscard]] PaneRect dividerRect(const andromeda::tiling::DwindleTree::Node& split, bool vertical, float gap, float hit);

  // The ratio for a divider dragged to `at` (px from the split's start), so each side keeps its smallest panes.
  [[nodiscard]] double dividerRatio(const andromeda::tiling::DwindleTree::Node& split, bool vertical, float at, float gap,
                                    float minWidth, float minHeight);

} // namespace ui
