// Random event sequences (open, "+", a session starts, the holder answers late, close, crash and restart, move,
// tear-off, close and reopen a session, split, leave, rotate) through WindowWorkspaces, a fake holder and each
// window's SessionList and SplitTrees keep the task's invariants once every answer is in.
#include "session_list.h"
#include "ui/split_layout.h"
#include "window_workspaces.h"
#include "workspace_bar.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <utility>

using namespace term;
using namespace term::session_event;
using ui::SplitTrees;
using andromeda::tiling::DwindleTree;

namespace {

int failures = 0;
unsigned seed = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s (seed %u)\n", __FILE__, __LINE__, #c, seed); ++failures; } } while (0)

struct Holder {
  std::set<std::uint64_t> workspaces;
  std::map<std::uint64_t, std::uint64_t> sessions; // held id -> workspace (0: none)
  std::uint64_t nextWorkspace = 1;
  std::uint64_t nextSession = 1;
  std::uint32_t nextReq = 1;
  std::deque<std::pair<std::uint32_t, std::uint64_t>> answers; // in flight
  bool reachable = true;

  void close(std::uint64_t ws) {
    workspaces.erase(ws);
    std::erase_if(sessions, [&](const auto& s) { return s.second == ws; });
  }
};

// One window's list and split trees, keyed by held id.
struct Panes {
  SessionList list;
  SplitTrees trees;
};

// What a closed session leaves for "Reopen closed": its window, list place and split partner.
struct Closed {
  int window = 0;
  std::uint64_t before = 0;
  std::uint64_t splitWith = 0;
  std::uint32_t edge = 0;
};

const SplitTrees::Room kRoom{.area = {.width = 1200, .height = 800}, .gap = 8, .minWidth = 200.0F};

struct Sim {
  Holder holder;
  std::map<int, Panes> panes;
  std::vector<Closed> closed; // newest last
  std::unique_ptr<WindowWorkspaces> model;
  std::map<int, std::set<std::uint64_t>> windows; // window -> its held sessions
  std::set<std::uint64_t> moved;                   // sessions that changed window
  int nextWindow = 1;
  std::mt19937 rng;

  explicit Sim(unsigned s) : rng(s) { start(); }

  WindowWorkspaces::Window key(int w) { return reinterpret_cast<WindowWorkspaces::Window>(static_cast<std::uintptr_t>(w)); }

  void start() {
    model = std::make_unique<WindowWorkspaces>(WindowWorkspaces::Port{
        .put = [this](const std::string&, const std::string&) -> std::optional<std::uint32_t> {
          if (!holder.reachable) return std::nullopt;
          const std::uint32_t req = holder.nextReq++;
          const std::uint64_t ws = holder.nextWorkspace++;
          holder.workspaces.insert(ws);
          holder.answers.emplace_back(req, ws);
          return req;
        },
        .setSession = [this](std::uint64_t held, std::uint64_t ws) {
          if ((ws == 0 || holder.workspaces.contains(ws)) && holder.sessions.contains(held)) holder.sessions[held] = ws;
        },
        .close = [this](std::uint64_t ws) { holder.close(ws); }});
  }

  std::uint64_t any(const std::set<std::uint64_t>& from) {
    return *std::next(from.begin(), static_cast<long>(std::uniform_int_distribution<std::size_t>(0, from.size() - 1)(rng)));
  }

  ApplyResult apply(int w, const SessionListEvent& event) {
    Panes& p = panes[w];
    const ApplyResult result = p.list.apply(event);
    p.trees.sync([&p](std::uint64_t id) { return p.list.splitOf(id); }, p.list.visible(), kRoom);
    return result;
  }

  // The window shows a session it gains, as openSession and adoptSession do.
  void attach(int w, std::uint64_t held) {
    windows[w].insert(held);
    apply(w, Open{held, SessionKind::Shell, "s", "/home/u"});
    apply(w, Show{held});
    model->sessionStarted(key(w), held, "/home/u");
  }

  std::uint64_t startSession(int w) {
    const std::uint64_t held = holder.nextSession++;
    holder.sessions[held] = 0;
    attach(w, held);
    return held;
  }

  void openWindow() {
    const int w = nextWindow++;
    windows[w];
    model->opened(key(w), std::nullopt);
    startSession(w);
  }

  void closeWindow(int w) {
    model->closed(key(w));
    for (const std::uint64_t held : windows[w]) holder.sessions.erase(held); // End::Close
    windows.erase(w);
    panes.erase(w);
  }

  // The source lets a session go; a last session's Close ends the window, as LastClosed does.
  void release(int w, std::uint64_t held) {
    windows[w].erase(held);
    const ApplyResult result = apply(w, Close{held});
    CHECK(result != ApplyResult::Rejected);
    CHECK((result == ApplyResult::LastClosed) == windows[w].empty());
    if (result == ApplyResult::LastClosed) closeWindow(w);
  }

  // "Move to window": the target adopts the live shell first; a source left empty then closes, as LastClosed does.
  void moveSession(int from, int to) {
    if (from == to || windows[from].empty()) return;
    const std::uint64_t held = any(windows[from]);
    moved.insert(held);
    attach(to, held);
    release(from, held);
  }

  // Tear-off (moveToNewWindow): the source closes the row first; menus and drags offer it only beside another session.
  void tearOff(int from) {
    if (windows[from].size() < 2) return;
    const std::uint64_t held = any(windows[from]);
    moved.insert(held);
    release(from, held);
    const int w = nextWindow++;
    windows[w];
    model->opened(key(w), std::nullopt);
    attach(w, held);
  }

  // A user close ends the shell and keeps what "Reopen closed" needs.
  void closeSession(int w) {
    if (windows[w].empty()) return;
    const std::uint64_t held = any(windows[w]);
    const SessionList& list = panes[w].list;
    Closed c{.window = w};
    const auto& all = list.sessions();
    const auto at = std::ranges::find(all, held, &SessionInfo::id);
    if (at + 1 != all.end()) c.before = (at + 1)->id;
    if (const auto* split = list.splitOf(held)) {
      c.splitWith = split->front() == held ? split->back() : split->front();
      c.edge = static_cast<std::uint32_t>(1U << std::uniform_int_distribution<int>(0, 3)(rng));
    }
    closed.push_back(c);
    if (closed.size() > 10) closed.erase(closed.begin());
    holder.sessions.erase(held);
    release(w, held);
  }

  // Reopen closed: a new shell in its window (else any), at its list place, joined to its partner on its side.
  void reopen() {
    if (closed.empty()) return;
    const Closed c = closed.back();
    closed.pop_back();
    int w = c.window;
    if (!windows.contains(w)) {
      if (windows.empty()) return;
      w = windows.begin()->first;
    }
    const std::uint64_t id = startSession(w);
    SessionList& list = panes[w].list;
    if (c.before != 0 && list.find(c.before) != nullptr) apply(w, Move{.id = id, .before = c.before});
    const auto* split = c.splitWith != 0 ? list.splitOf(c.splitWith) : nullptr;
    if (c.splitWith == 0 || list.find(c.splitWith) == nullptr || (split != nullptr && split->size() >= kMaxPanes)) return;
    panes[w].trees.pendJoin({.target = c.splitWith, .joining = id, .edge = c.edge});
    const bool before = c.edge == andromeda::tiling::EdgeLeft || c.edge == andromeda::tiling::EdgeTop;
    CHECK(apply(w, Join{.target = c.splitWith, .joining = id, .before = before}) == ApplyResult::Applied);
    const auto* now = list.splitOf(id);
    CHECK(now != nullptr && std::ranges::find(*now, c.splitWith) != now->end());
    const DwindleTree* tree = panes[w].trees.treeOf(id);
    CHECK(tree != nullptr && tree->find(c.splitWith) != nullptr);
  }

  // Split from a drop (an edge) or the menu (the tree picks the side), as rowDrag and joinShown do.
  void split(int w) {
    if (windows[w].size() < 2) return;
    const std::uint64_t target = any(windows[w]);
    const std::uint64_t joining = any(windows[w]);
    SessionList& list = panes[w].list;
    const auto edge = std::uniform_int_distribution<int>(0, 1)(rng) == 0 ? 0U : static_cast<std::uint32_t>(1U << std::uniform_int_distribution<int>(0, 3)(rng));
    if (list.shown() && list.splitOf(joining) == nullptr) panes[w].trees.pendJoin({.target = target, .joining = joining, .edge = edge});
    apply(w, Join{target, joining, edge == andromeda::tiling::EdgeLeft || edge == andromeda::tiling::EdgeTop});
  }

  void leave(int w) {
    if (!windows[w].empty()) apply(w, Leave{any(windows[w])});
  }

  void show(int w) {
    if (!windows[w].empty()) apply(w, Show{any(windows[w])});
  }

  void rotate(int w) {
    const auto shown = panes[w].list.shown();
    if (!shown) return;
    const DwindleTree* tree = panes[w].trees.treeOf(*shown);
    auto keys = tree != nullptr ? tree->keys() : decltype(tree->keys()){};
    CHECK(panes[w].trees.rotate(*shown) == (tree != nullptr));
    if (tree != nullptr) CHECK(panes[w].trees.treeOf(*shown) != nullptr && panes[w].trees.treeOf(*shown)->keys() == keys);
  }

  void answerOne() {
    if (holder.answers.empty()) return;
    // Answers come back in order on one connection; a late one is still in order.
    const auto [req, ws] = holder.answers.front();
    holder.answers.pop_front();
    model->answered(req, ws);
  }

  // The holder connection drops with answers in flight; the reconnect's first table closes the empty workspaces.
  void dropConnection() {
    for (const auto& [req, ws] : std::exchange(holder.answers, {})) model->lost(req);
    for (std::uint64_t ws : std::set(holder.workspaces)) {
      if (std::ranges::none_of(holder.sessions, [&](const auto& s) { return s.second == ws; })) holder.close(ws);
    }
  }

  // The app dies: windows go, shells and workspaces stay. The next start does what TerminalApp::activate does.
  void crashAndRestart() {
    windows.clear();
    panes.clear();
    closed.clear();
    holder.answers.clear(); // the connection dropped
    start();
    for (std::uint64_t ws : std::set(holder.workspaces)) {
      if (std::ranges::none_of(holder.sessions, [&](const auto& s) { return s.second == ws; })) holder.close(ws);
    }
    std::map<std::uint64_t, std::vector<std::uint64_t>> groups;
    for (const auto& [held, ws] : holder.sessions) groups[ws].push_back(held);
    for (const auto& [ws, held] : groups) {
      const int w = nextWindow++;
      model->opened(key(w), ws == 0 ? std::nullopt : std::optional(ws));
      for (const std::uint64_t h : held) attach(w, h);
    }
  }

  void step() {
    std::vector<int> open;
    for (const auto& [w, s] : windows) open.push_back(w);
    const int pick = open.empty() ? 0 : open[std::uniform_int_distribution<std::size_t>(0, open.size() - 1)(rng)];
    const int action = std::uniform_int_distribution<int>(0, 19)(rng);
    if (pick == 0 && action != 0 && action != 1 && action != 4 && action < 16) return;
    switch (action) {
    case 0: case 1: openWindow(); break; // launch or "+"
    case 2: startSession(pick); break;
    case 3: closeWindow(pick); break;
    case 4: crashAndRestart(); break;
    case 5: holder.reachable = !holder.reachable; break;
    case 6: dropConnection(); break;
    case 7: moveSession(pick, open[std::uniform_int_distribution<std::size_t>(0, open.size() - 1)(rng)]); break;
    case 8: tearOff(pick); break;
    case 9: closeSession(pick); break;
    case 10: reopen(); break;
    case 11: case 12: split(pick); break;
    case 13: leave(pick); break;
    case 14: rotate(pick); break;
    case 15: show(pick); break;
    default: answerOne(); break;
    }
    checkWindows();
  }

  void settle() {
    holder.reachable = true;
    while (!holder.answers.empty()) answerOne();
    // A window whose put was refused joins on its next session.
    for (auto& [w, held] : windows) {
      if (!model->workspaceOf(key(w))) startSession(w);
    }
    while (!holder.answers.empty()) answerOne();
  }

  // Each session is in exactly one window's list; each window's trees match its splits. Holds after every step.
  void checkWindows() {
    std::set<std::uint64_t> seen;
    CHECK(panes.size() == windows.size());
    for (const auto& [w, held] : windows) {
      CHECK(!held.empty());
      const Panes& p = panes.at(w);
      std::set<std::uint64_t> listed;
      for (const SessionInfo& s : p.list.sessions()) {
        listed.insert(s.id);
        CHECK(seen.insert(s.id).second);
      }
      CHECK(listed == held);
      std::set<std::uint64_t> inTree;
      for (const DwindleTree& tree : p.trees.trees()) {
        const auto keys = tree.keys();
        CHECK(keys.size() >= 2);
        for (const auto k : keys) {
          CHECK(inTree.insert(k).second);
          CHECK(p.list.splitOf(k) != nullptr && p.list.splitOf(k) == p.list.splitOf(keys.front()));
        }
      }
      const auto shown = p.list.visible();
      if (shown.size() >= 2) {
        const DwindleTree* own = p.trees.treeOf(shown.front());
        CHECK(own != nullptr);
        if (own == nullptr) continue;
        auto keys = own->keys();
        std::ranges::sort(keys);
        auto want = shown;
        std::ranges::sort(want);
        CHECK(std::ranges::equal(keys, want));
      }
    }
  }

  void checkInvariants() {
    std::set<std::uint64_t> owned;
    for (const auto& [w, held] : windows) {
      const auto ws = model->workspaceOf(key(w));
      CHECK(ws.has_value());
      if (!ws) continue;
      CHECK(holder.workspaces.contains(*ws));
      CHECK(owned.insert(*ws).second); // one window per workspace
      for (const std::uint64_t h : held) CHECK(!holder.sessions.contains(h) || holder.sessions.at(h) == *ws);
    }
    CHECK(owned == holder.workspaces); // no workspace without a window
    // A moved shell lives on in its new window: closing the window it left does not end it.
    for (const auto& [w, held] : windows) {
      for (const std::uint64_t h : held) CHECK(!moved.contains(h) || holder.sessions.contains(h));
    }
    for (const auto& [held, ws] : holder.sessions) {
      CHECK(std::ranges::any_of(windows, [&](const auto& w) { return w.second.contains(held); })); // no shell unseen
    }
    std::vector<workspace_bar::Window> shown;
    for (const auto& [w, held] : windows) shown.push_back({.id = static_cast<std::uint64_t>(w), .workspace = model->workspaceOf(key(w)), .name = std::format("w{}", w)});
    holder::Workspaces table;
    for (const std::uint64_t ws : holder.workspaces) table.workspaces.push_back({.id = ws, .name = "u", .root = "/home/u"});
    for (const auto& [w, held] : windows) {
      const auto bar = workspace_bar::barFor(table, shown, static_cast<std::uint64_t>(w));
      CHECK(bar.size() == (windows.size() < 2 ? 0 : windows.size()));
      CHECK(windows.size() < 2 || std::ranges::count(bar, true, &workspace_bar::Item::here) == 1);
      // Each item is told apart: its window's own name, and a mark no other item has (up to 9 windows).
      std::set<std::string> marks;
      for (std::size_t i = 0; i < bar.size(); ++i) {
        CHECK(bar[i].name == shown[i].name);
        marks.insert(bar[i].symbol);
      }
      CHECK(bar.size() > 9 || marks.size() == bar.size());
    }
  }
};

} // namespace

int main() {
  for (seed = 1; seed <= 300; ++seed) {
    Sim sim(seed);
    for (int i = 0; i < 200; ++i) {
      sim.step();
      if (i % 25 == 24) {
        sim.settle();
        sim.checkInvariants();
      }
    }
    sim.settle();
    sim.checkInvariants();
  }
  if (failures > 0) return 1;
  std::puts("workspace model: ok");
  return 0;
}
