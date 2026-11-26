// Random lists and splits, then a close of random sessions and "Reopen closed" of each, through closedItems and
// reopenSteps: the list comes back in its old order, its splits with the same members, and every pane on its old tile.
#include "session_layout.h"
#include "session_list.h"
#include "ui/split_layout.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace term;
using namespace term::session_event;
using ui::SplitTrees;
using andromeda::tiling::DwindleTree;
using andromeda::tiling::Tile;
using andromeda::tiling::TileBox;

namespace {

int failures = 0;
unsigned seed = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s (seed %u)\n", __FILE__, __LINE__, #c, seed); ++failures; } } while (0)

const SplitTrees::Room kRoom{.area = {.width = 1200, .height = 800}, .gap = 8, .minWidth = 200.0F};

struct Window {
  SessionList list;
  SplitTrees trees;
  std::uint64_t nextId = 1;

  void sync() {
    trees.sync([this](std::uint64_t id) { return list.splitOf(id); }, list.visible(), kRoom);
  }
  void apply(const SessionListEvent& event) {
    list.apply(event);
    sync();
  }
  std::uint64_t open(const std::string& name) {
    const std::uint64_t id = nextId++;
    apply(Open{id, SessionKind::Shell, name, "/tmp"});
    apply(Show{id});
    return id;
  }
  // As TerminalWindow::reopenSession: a new shell, then the steps, the join pended right before its Join.
  std::uint64_t reopen(const ClosedSession& closed) {
    const std::uint64_t id = open("");
    const ReopenSteps steps = reopenSteps(list, closed, id);
    for (const SessionListEvent& event : steps.events) {
      if (steps.join && std::holds_alternative<Join>(event)) {
        trees.pendJoin({.target = steps.join->target, .joining = id, .edge = steps.join->edge, .shape = steps.join->shape});
      }
      apply(event);
    }
    return id;
  }
  std::vector<std::string> names() const {
    std::vector<std::string> out;
    for (const SessionInfo& s : list.sessions()) out.push_back(s.name);
    return out;
  }
  const SessionInfo* named(const std::string& name) const {
    const auto it = std::ranges::find(list.sessions(), name, &SessionInfo::name);
    return it != list.sessions().end() ? &*it : nullptr;
  }
  // Each split as the sorted names of its members.
  std::vector<std::vector<std::string>> splits() const {
    std::vector<std::vector<std::string>> out;
    for (const auto& split : list.splits()) {
      std::vector<std::string> members;
      for (const auto id : split) members.push_back(list.find(id)->name);
      std::ranges::sort(members);
      out.push_back(members);
    }
    std::ranges::sort(out);
    return out;
  }
  // A session's tile in its split's tree, by name.
  std::optional<TileBox> tile(const std::string& name) const {
    const SessionInfo* s = named(name);
    const DwindleTree* tree = s != nullptr ? trees.treeOf(s->id) : nullptr;
    if (tree == nullptr) return std::nullopt;
    DwindleTree copy;
    copy.setShape(tree->shape());
    const auto tiles = copy.arrange(kRoom.area, kRoom.gap, true);
    const auto it = std::ranges::find(tiles, s->id, &Tile::key);
    return it != tiles.end() ? std::optional(it->box) : std::nullopt;
  }
};

bool same(const std::optional<TileBox>& a, const std::optional<TileBox>& b) {
  return a.has_value() == b.has_value() &&
         (!a || (a->x == b->x && a->y == b->y && a->width == b->width && a->height == b->height));
}

} // namespace

int main() {
  for (seed = 1; seed <= 400; ++seed) {
    std::mt19937 rng(seed);
    const auto pick = [&rng](std::size_t n) { return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng); };
    Window w;
    const std::size_t count = 2 + pick(5);
    for (std::size_t i = 0; i < count; ++i) w.open("n" + std::to_string(i));
    // Splits by drop (an edge) or by the menu, as the window makes them.
    for (int j = 0, n = static_cast<int>(pick(6)); j < n; ++j) {
      const auto& all = w.list.sessions();
      const std::uint64_t target = all[pick(all.size())].id;
      const std::uint64_t joining = all[pick(all.size())].id;
      w.apply(Show{target});
      if (w.list.splitOf(joining) != nullptr) continue;
      const auto edge = static_cast<std::uint32_t>(pick(2) == 0 ? 0U : 1U << pick(4));
      w.trees.pendJoin({.target = target, .joining = joining, .edge = edge});
      w.apply(Join{target, joining, edge == andromeda::tiling::EdgeLeft || edge == andromeda::tiling::EdgeTop});
    }
    if (pick(3) == 0 && w.trees.trees().size() > 0) {
      const auto keys = w.trees.trees().front().keys();
      w.trees.rotate(keys.front());
    }
    // Close one or more, never all: a closed window is a restore, which split_trees checks.
    std::vector<std::uint64_t> ids;
    for (const SessionInfo& s : w.list.sessions()) {
      if (pick(3) == 0) ids.push_back(s.id);
    }
    if (ids.empty()) ids.push_back(w.list.sessions()[pick(w.list.sessions().size())].id);
    if (ids.size() == w.list.sessions().size()) ids.pop_back();
    const auto order = w.names();
    const auto splits = w.splits();
    std::map<std::string, std::optional<TileBox>> tiles;
    for (const auto& name : order) tiles[name] = w.tile(name);

    std::deque<ClosedItem> stack; // newest first, as TerminalApp::pushClosed
    for (ClosedItem& item : closedItems(w.list, ids, 1, std::nullopt, [](std::uint64_t) { return std::string("/tmp"); },
                                        [&w](std::uint64_t id) { return w.trees.treeOf(id); })) {
      stack.push_front(std::move(item));
    }
    CHECK(stack.size() == ids.size());
    for (const auto id : ids) w.apply(Close{id});
    // Other sessions show meanwhile; the reopen does not depend on what the window shows.
    w.apply(Show{w.list.sessions()[pick(w.list.sessions().size())].id});
    while (!stack.empty()) {
      const auto* closed = std::get_if<ClosedSession>(&stack.front());
      CHECK(closed != nullptr);
      const ClosedItem item = std::move(stack.front());
      stack.pop_front();
      if (closed == nullptr) continue;
      const auto& was = std::get<ClosedSession>(item);
      const std::uint64_t id = w.reopen(was);
      for (ClosedItem& other : stack) renameClosed(other, was.id, id);
    }
    CHECK(w.names() == order);
    CHECK(w.splits() == splits);
    for (const auto& n : order) CHECK(same(w.tile(n), tiles[n]));
  }
  if (failures > 0) return 1;
  std::puts("reopen closed: ok");
  return 0;
}
