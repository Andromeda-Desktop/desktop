#include "session_list.h"
#include "ui/split_layout.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <optional>
#include <set>

using namespace term;
using ui::SplitTrees;
using namespace term::session_event;
using andromeda::tiling::DwindleTree;
using andromeda::tiling::Tile;

namespace {

int failures = 0;
unsigned seed = 0;
int step = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s (seed %u step %d)\n", __FILE__, __LINE__, #c, seed, step); ++failures; } } while (0)

SessionListEvent randomEvent(std::mt19937& rng) {
  const std::uint64_t id = std::uniform_int_distribution<std::uint64_t>(1, 6)(rng);
  const std::uint64_t other = std::uniform_int_distribution<std::uint64_t>(1, 6)(rng);
  switch (std::uniform_int_distribution<int>(0, 7)(rng)) {
  case 0: return Open{id, SessionKind::Shell, "s", "~"};
  case 1: return Close{id};
  case 2: return Show{id};
  case 3:
  case 4: return Join{id, other, std::uniform_int_distribution<int>(0, 1)(rng) == 0};
  case 5: return Swap{id, other};
  default: return Leave{id};
  }
}

void checkInvariants(const SessionList& list, const SplitTrees& trees, const DwindleTree* own, const std::vector<std::uint64_t>& split) {
  // The shown split's tree holds exactly its sessions; a lone session has none.
  CHECK((own == nullptr) == (split.size() < 2));
  if (own != nullptr) {
    auto keys = own->keys();
    std::ranges::sort(keys);
    decltype(keys) want(split.begin(), split.end());
    std::ranges::sort(want);
    CHECK(keys == want);
  }
  // Every tree has two or more sessions, all from one live split, and a session is in at most one tree.
  std::set<std::uint64_t> seen;
  for (const DwindleTree& tree : trees.trees()) {
    const auto keys = tree.keys();
    CHECK(keys.size() >= 2);
    for (const auto key : keys) {
      CHECK(seen.insert(key).second);
      CHECK(list.splitOf(key) != nullptr && list.splitOf(key) == list.splitOf(keys.front()));
    }
  }
}

} // namespace

// The app syncs only while a split shows: a split left and joined again on another edge must take the new edge.
void rejoinOnNewEdge() {
  SessionList list;
  SplitTrees trees;
  const SplitTrees::Room room{.area = {.width = 1200, .height = 800}, .gap = 8, .minWidth = 200.0F};
  const auto splitOf = [&list](std::uint64_t id) { return list.splitOf(id); };
  list.apply(Open{1, SessionKind::Shell, "s", "~"});
  list.apply(Open{2, SessionKind::Shell, "s", "~"});
  list.apply(Show{1});
  trees.pendJoin({.target = 1, .joining = 2, .edge = andromeda::tiling::EdgeRight});
  list.apply(Join{1, 2, false});
  CHECK(trees.sync(splitOf, list.visible(), room) != nullptr);
  list.apply(Leave{2});
  list.apply(Show{1});
  trees.pendJoin({.target = 1, .joining = 2, .edge = andromeda::tiling::EdgeBottom});
  list.apply(Join{1, 2, false});
  DwindleTree* own = trees.sync(splitOf, list.visible(), room);
  CHECK(own != nullptr);
  if (own != nullptr) {
    const auto tiles = own->arrange(room.area, room.gap, true);
    const auto at = [&tiles](std::uint64_t key) { return std::ranges::find(tiles, key, &Tile::key)->box; };
    CHECK(at(2).y > at(1).y && at(2).x == at(1).x);
  }
}

int main() {
  rejoinOnNewEdge();
  for (seed = 1; seed <= 300; ++seed) {
    std::mt19937 rng(seed);
    SessionList list;
    SplitTrees trees;
    for (step = 0; step < 300; ++step) {
      const SessionListEvent event = randomEvent(rng);
      const auto* join = std::get_if<Join>(&event);
      const auto edge = static_cast<std::uint32_t>(1U << std::uniform_int_distribution<int>(0, 3)(rng));
      const bool dropped = join != nullptr && std::uniform_int_distribution<int>(0, 1)(rng) == 0;
      // A drop joins the session the window shows.
      // A menu split (edge 0) lets the tree pick the side.
      if (join != nullptr && list.shown() && list.splitOf(join->joining) == nullptr) {
        trees.pendJoin({.target = join->target, .joining = join->joining, .edge = dropped ? edge : 0U});
      }
      const SplitTrees::Room room{.area = {.width = 1200, .height = 800}, .gap = 8, .minWidth = 200.0F};
      const auto splitOf = [&list](std::uint64_t id) { return list.splitOf(id); };
      const auto* swap = std::get_if<Swap>(&event);
      // Where both panes of a swap sit before it, when the window shows their split.
      std::optional<std::pair<andromeda::tiling::TileBox, andromeda::tiling::TileBox>> before;
      if (DwindleTree* shown = swap != nullptr ? trees.sync(splitOf, list.visible(), room) : nullptr;
          shown != nullptr && shown->find(swap->a) != nullptr && shown->find(swap->b) != nullptr && swap->a != swap->b) {
        const auto tiles = shown->arrange(room.area, room.gap, true);
        const auto at = [&tiles](std::uint64_t key) { return std::ranges::find(tiles, key, &Tile::key)->box; };
        before = std::pair{at(swap->a), at(swap->b)};
      }
      const bool accepted = list.apply(event) != ApplyResult::Rejected;
      if (swap != nullptr && accepted) {
        trees.swap(swap->a, swap->b);
      }
      const auto split = list.visible();
      DwindleTree* own = trees.sync(splitOf, split, room);
      checkInvariants(list, trees, own, split);
      if (own == nullptr) {
        continue;
      }
      // The tiles cover the area without overlap, one per session.
      const std::vector<Tile> tiles = own->arrange({.width = 1200, .height = 800}, 8, true);
      CHECK(tiles.size() == split.size());
      for (std::size_t i = 0; i < tiles.size(); ++i) {
        CHECK(tiles[i].box.width > 0 && tiles[i].box.height > 0);
        for (std::size_t k = i + 1; k < tiles.size(); ++k) {
          const auto& a = tiles[i].box;
          const auto& b = tiles[k].box;
          CHECK(a.x + a.width <= b.x || b.x + b.width <= a.x || a.y + a.height <= b.y || b.y + b.height <= a.y);
        }
      }
      // A menu split goes beside its target when both halves keep the minimum width.
      if (join != nullptr && !dropped && accepted) {
        const auto at = [&tiles](std::uint64_t key) { return std::ranges::find(tiles, key, &Tile::key)->box; };
        const auto t = at(join->target);
        const auto j = at(join->joining);
        const int whole = std::max(j.x + j.width - t.x, std::max(t.width, j.width));
        if (static_cast<float>(whole - room.gap) / 2.0F >= room.minWidth) {
          CHECK(j.y == t.y && j.x > t.x);
        } else {
          CHECK(j.x == t.x && j.y > t.y);
        }
      }
      // A swap of two shown panes trades their tiles exactly.
      if (before && accepted) {
        const auto at = [&tiles](std::uint64_t key) { return std::ranges::find(tiles, key, &Tile::key)->box; };
        const auto same = [](const andromeda::tiling::TileBox& x, const andromeda::tiling::TileBox& y) {
          return x.x == y.x && x.y == y.y && x.width == y.width && x.height == y.height;
        };
        CHECK(same(at(swap->a), before->second) && same(at(swap->b), before->first));
      }
      CHECK(!(before && !accepted));
      // A dropped session lands on the edge of its target it was dropped on.
      if (dropped && accepted) {
        const auto at = [&tiles](std::uint64_t key) { return std::ranges::find(tiles, key, &Tile::key)->box; };
        const auto t = at(join->target);
        const auto j = at(join->joining);
        if (edge == andromeda::tiling::EdgeLeft) CHECK(j.x < t.x);
        if (edge == andromeda::tiling::EdgeRight) CHECK(j.x > t.x);
        if (edge == andromeda::tiling::EdgeTop) CHECK(j.y < t.y);
        if (edge == andromeda::tiling::EdgeBottom) CHECK(j.y > t.y);
      }
    }
  }
  if (failures == 0) {
    std::puts("split_trees: ok");
  }
  return failures == 0 ? 0 : 1;
}
