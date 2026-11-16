#include "split_trees.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <set>

using namespace term;
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
  switch (std::uniform_int_distribution<int>(0, 6)(rng)) {
  case 0: return Open{id, SessionKind::Shell, "s", "~"};
  case 1: return Close{id};
  case 2: return Show{id};
  case 3:
  case 4: return Join{id, other, std::uniform_int_distribution<int>(0, 1)(rng) == 0};
  default: return Leave{id};
  }
}

void checkInvariants(const SessionList& list, const SplitTrees& trees, const DwindleTree* own, const std::vector<std::uint64_t>& split) {
  // The shown split's tree holds exactly its sessions; a lone session has none.
  CHECK((own == nullptr) == (split.size() < 2));
  if (own != nullptr) {
    auto keys = own->keys();
    std::ranges::sort(keys);
    auto want = split;
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

int main() {
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
      if (join != nullptr && list.shown()) {
        trees.pendJoin({.target = join->target, .joining = join->joining, .edge = dropped ? edge : 0U});
      }
      const bool accepted = list.apply(event) != ApplyResult::Rejected;
      const auto split = list.visible();
      const SplitTrees::Room room{.area = {.width = 1200, .height = 800}, .gap = 8, .minWidth = 200.0F};
      DwindleTree* own = trees.sync(list, split, room);
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
