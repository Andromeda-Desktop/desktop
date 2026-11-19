// Random event sequences (open, "+", a session starts, the holder answers late, close, crash and restart) through
// WindowWorkspaces and a fake holder keep the task's invariants once every answer is in.
#include "window_workspaces.h"
#include "workspace_bar.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <set>

using namespace term;

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

struct Sim {
  Holder holder;
  std::unique_ptr<WindowWorkspaces> model;
  std::map<int, std::set<std::uint64_t>> windows; // window -> its held sessions
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
          if (holder.workspaces.contains(ws) && holder.sessions.contains(held)) holder.sessions[held] = ws;
        },
        .close = [this](std::uint64_t ws) { holder.close(ws); }});
  }

  void startSession(int w) {
    const std::uint64_t held = holder.nextSession++;
    holder.sessions[held] = 0;
    windows[w].insert(held);
    model->sessionStarted(key(w), held, "/home/u");
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
  }

  void answerOne() {
    if (holder.answers.empty()) return;
    // Answers come back in order on one connection; a late one is still in order.
    const auto [req, ws] = holder.answers.front();
    holder.answers.pop_front();
    model->answered(req, ws);
  }

  // The app dies: windows go, shells and workspaces stay. The next start does what TerminalApp::activate does.
  void crashAndRestart() {
    windows.clear();
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
      for (const std::uint64_t h : held) {
        windows[w].insert(h);
        model->sessionStarted(key(w), h, "/home/u");
      }
    }
  }

  void step() {
    std::vector<int> open;
    for (const auto& [w, s] : windows) open.push_back(w);
    const int pick = open.empty() ? 0 : open[std::uniform_int_distribution<std::size_t>(0, open.size() - 1)(rng)];
    switch (std::uniform_int_distribution<int>(0, 9)(rng)) {
    case 0: case 1: openWindow(); break; // launch or "+"
    case 2: if (pick != 0) startSession(pick); break;
    case 3: if (pick != 0) closeWindow(pick); break;
    case 4: crashAndRestart(); break;
    case 5: holder.reachable = !holder.reachable; break;
    default: answerOne(); break;
    }
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
    for (const auto& [held, ws] : holder.sessions) {
      CHECK(std::ranges::any_of(windows, [&](const auto& w) { return w.second.contains(held); })); // no shell unseen
    }
    std::vector<workspace_bar::Window> shown;
    for (const auto& [w, held] : windows) shown.push_back({.id = static_cast<std::uint64_t>(w), .workspace = model->workspaceOf(key(w))});
    holder::Workspaces table;
    for (const std::uint64_t ws : holder.workspaces) table.workspaces.push_back({.id = ws, .name = "u", .root = "/home/u"});
    for (const auto& [w, held] : windows) {
      const auto bar = workspace_bar::barFor(table, shown, static_cast<std::uint64_t>(w));
      CHECK(bar.size() == (windows.size() < 2 ? 0 : windows.size()));
      CHECK(windows.size() < 2 || std::ranges::count(bar, true, &workspace_bar::Item::here) == 1);
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
