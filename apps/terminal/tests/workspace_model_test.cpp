// Random event sequences over the workspace table, the windows' foot bars and their pagers (the TerminalApp and
// TerminalWindow rules on top of barFor and WorkspacePager) keep the task's invariants.
#include "workspace_bar.h"
#include "workspace_pager.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <random>
#include <set>

using namespace term;
using namespace term::workspace_bar;

namespace {

int failures = 0;
unsigned seed = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s (seed %u)\n", __FILE__, __LINE__, #c, seed); ++failures; } } while (0)

constexpr float kWidth = 600.0F;

struct Window {
  std::uint64_t here = 0;
  Bar bar;
  WorkspacePager pager;
};

struct App {
  holder::Workspaces table;
  std::map<int, Window> windows;
  std::uint64_t nextId = 1;
  int nextWindow = 1;

  std::set<std::uint64_t> elsewhere(int w) const {
    std::set<std::uint64_t> out;
    for (const auto& [id, window] : windows) {
      if (id != w && window.here != 0) out.insert(window.here);
    }
    return out;
  }
  bool exists(std::uint64_t id) const { return std::ranges::contains(table.workspaces, id, &holder::WorkspaceInfo::id); }

  // TerminalWindow::fillFootBar: the pager restarts only when its pages or this window's page change.
  void sync() {
    for (auto& [id, window] : windows) {
      Bar bar = barFor(table, window.here, elsewhere(id));
      if (bar.pages != window.bar.pages || bar.here != window.bar.here) window.pager.setPages(bar.pages.size(), bar.here);
      window.bar = std::move(bar);
    }
  }

  std::uint64_t put() {
    const std::uint64_t id = nextId++;
    table.workspaces.push_back({.id = id, .name = "w"});
    return id;
  }
  void addSession(std::uint64_t workspace, bool attached) {
    table.sessions.push_back({.id = nextId++, .workspace = workspace, .attached = attached});
  }
  void close(std::uint64_t id) {
    std::erase_if(table.workspaces, [id](const auto& w) { return w.id == id; });
    std::erase_if(table.sessions, [id](const auto& s) { return s.workspace == id; });
  }
  bool running(std::uint64_t id) const {
    return std::ranges::any_of(table.sessions, [id](const auto& s) { return s.workspace == id && s.command == holder::CommandState::Running; });
  }
  void detach(std::uint64_t id) {
    for (auto& s : table.sessions) {
      if (s.workspace == id) s.attached = false;
    }
  }

  void newWindow() {
    Window window;
    window.pager.setWidth(kWidth);
    window.here = put();
    addSession(window.here, true);
    windows.emplace(nextWindow++, std::move(window));
  }
  // TerminalApp::pickWorkspace + leaveWorkspace.
  void pick(int w, std::uint64_t id) {
    Window& window = windows.at(w);
    if (elsewhere(w).contains(id) || !exists(id) || id == window.here) return;
    const std::uint64_t left = window.here;
    detach(left);
    window.here = id;
    bool any = false;
    for (auto& s : table.sessions) {
      if (s.workspace == id) any = s.attached = true;
    }
    if (!any) addSession(id, true);
    if (left != 0 && !running(left)) close(left);
  }
  // TerminalApp::windowClosed.
  void closeWindow(int w) {
    const std::uint64_t here = windows.at(w).here;
    detach(here);
    if (here != 0 && !running(here)) close(here);
    windows.erase(w);
  }
  // A swipe of `dx` px in `steps` events, then the release, the settle and the landing (TerminalWindow::pagerLanded).
  void swipe(int w, float dx, int steps, std::uint32_t& time, bool statusChangeMidway, std::mt19937& rng) {
    Window& window = windows.at(w);
    if (window.pager.count() < 2) return;
    for (int i = 0; i < steps; ++i) {
      window.pager.track(dx / static_cast<float>(steps), time += 8);
      if (statusChangeMidway && i == steps / 2 && !table.sessions.empty()) {
        auto& s = table.sessions[rng() % table.sessions.size()];
        s.command = s.command == holder::CommandState::Running ? holder::CommandState::Done : holder::CommandState::Running;
        sync();
        CHECK(std::holds_alternative<WorkspacePager::Tracking>(windows.at(w).pager.mode()));
      }
    }
    Window& still = windows.at(w);
    const std::size_t target = still.pager.release();
    CHECK(target < still.pager.count());
    still.pager.animate(static_cast<float>(target) * kWidth);
    still.pager.settled();
    CHECK(still.pager.page() == target);
    if (target != still.bar.here && target < still.bar.pages.size()) {
      const std::uint64_t id = still.bar.pages[target];
      CHECK(!elsewhere(w).contains(id));
      pick(w, id);
      CHECK(still.here == id);
    }
  }
};

void checkInvariants(const App& app) {
  std::set<std::uint64_t> shown;
  for (const auto& [w, window] : app.windows) {
    const Bar& bar = window.bar;
    const std::size_t n = app.table.workspaces.size();
    if (n >= 2) CHECK(bar.items.size() + bar.more == n);
    if (window.here != 0 && app.exists(window.here)) CHECK(shown.insert(window.here).second);
    // One workspace: no pages, the sidebar as before.
    if (n >= 2 && window.here != 0 && app.exists(window.here)) {
      CHECK(bar.here < bar.pages.size() && bar.pages[bar.here] == window.here);
      const auto others = app.elsewhere(w);
      for (const auto& ws : app.table.workspaces) CHECK(std::ranges::contains(bar.pages, ws.id) != others.contains(ws.id));
    } else {
      CHECK(bar.pages.empty());
    }
    CHECK(window.pager.count() == std::max<std::size_t>(bar.pages.size(), 1));
    CHECK(window.pager.page() < window.pager.count());
    if (std::holds_alternative<WorkspacePager::Rest>(window.pager.mode())) {
      CHECK(window.pager.position() == static_cast<float>(window.pager.page()) * kWidth);
      CHECK(bar.pages.empty() || window.pager.page() == bar.here);
    }
  }
}

} // namespace

int main() {
  for (seed = 1; seed <= 1500; ++seed) {
    std::mt19937 rng(seed);
    auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
    App app;
    std::uint32_t time = 0;
    app.newWindow();
    app.sync();
    for (int step = 0; step < 80; ++step) {
      const auto windowIds = [&] {
        std::vector<int> ids;
        for (const auto& [id, window] : app.windows) ids.push_back(id);
        return ids;
      }();
      const int w = windowIds.empty() ? 0 : windowIds[static_cast<std::size_t>(pick(static_cast<int>(windowIds.size())))];
      switch (pick(8)) {
        case 0: app.newWindow(); break;
        case 1: app.put(); break; // a saved workspace comes back
        case 2:
          if (w != 0) app.closeWindow(w);
          break;
        case 3: // close a workspace no window shows
          if (!app.table.workspaces.empty()) {
            const std::uint64_t id = app.table.workspaces[static_cast<std::size_t>(pick(static_cast<int>(app.table.workspaces.size())))].id;
            if (!app.elsewhere(0).contains(id)) app.close(id);
          }
          break;
        case 4: // ptyd restart: shells gone, the saved workspaces come back under new ids
          app.table.sessions.clear();
          for (auto& ws : app.table.workspaces) ws.id = app.nextId++;
          break;
        case 5:
          if (w != 0 && !app.table.workspaces.empty()) {
            app.pick(w, app.table.workspaces[static_cast<std::size_t>(pick(static_cast<int>(app.table.workspaces.size())))].id);
          }
          break;
        default:
          if (w != 0) app.swipe(w, static_cast<float>(pick(2400) - 1200), 1 + pick(12), time, pick(2) == 0, rng);
          break;
      }
      if (app.windows.empty()) app.newWindow();
      app.sync();
      checkInvariants(app);
    }
  }
  std::printf("%s\n", failures == 0 ? "workspace model: ok" : "workspace model: FAILED");
  return failures == 0 ? 0 : 1;
}
