#pragma once

#include "protocols/dwindle_tree.h"
#include "session_list.h"

#include <cstdint>
#include <list>
#include <optional>
#include <vector>

namespace term {

  // The layout trees of a window's splits (the compositor's dwindle tree); their leaves are session ids.
  class SplitTrees {
  public:
    struct Join {
      std::uint64_t target = 0;
      std::uint64_t joining = 0;
      std::uint32_t edge = 0; // a TileEdge; 0: the tree picks the side
    };
    // The next sync puts `joining` on `edge` of `target`.
    void pendJoin(const Join& join) { m_pending = join; }
    // The tree of `split`, with the sessions that left it removed and the ones that joined it inserted; the other
    // trees lose sessions that are no longer in a split. Null for a lone session.
    andromeda::tiling::DwindleTree* sync(const SessionList& sessions, const std::vector<std::uint64_t>& split);
    [[nodiscard]] const std::list<andromeda::tiling::DwindleTree>& trees() const { return m_trees; }

  private:
    std::list<andromeda::tiling::DwindleTree> m_trees; // a list: erasing a tree keeps the others' nodes in place
    std::optional<Join> m_pending;
  };

} // namespace term
