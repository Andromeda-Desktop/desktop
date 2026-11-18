#include "workspace_bar.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <set>

using namespace term;
using namespace term::workspace_bar;

namespace {

int failures = 0;
unsigned seed = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s (seed %u)\n", __FILE__, __LINE__, #c, seed); ++failures; } } while (0)

void checkBar(const holder::Workspaces& table, std::uint64_t here, const std::set<std::uint64_t>& elsewhere) {
  const Bar bar = barFor(table, here, elsewhere);
  const std::size_t n = table.workspaces.size();
  if (n < 2) {
    CHECK(bar.items.empty() && bar.more == 0);
    return;
  }
  CHECK(bar.items.size() + bar.more == n);
  CHECK(bar.items.size() == (n <= kMaxItems ? n : kMaxItems - 1));
  const bool hereKnown = std::ranges::any_of(table.workspaces, [&](const auto& w) { return w.id == here; });
  CHECK(std::ranges::count(bar.items, Location::Here, &Item::location) == (hereKnown ? 1 : 0));
  std::set<std::uint64_t> ids;
  std::size_t last = 0;
  for (const Item& item : bar.items) {
    CHECK(ids.insert(item.id).second);
    const auto pos = static_cast<std::size_t>(std::ranges::find(table.workspaces, item.id, &holder::WorkspaceInfo::id) - table.workspaces.begin());
    CHECK(pos < n && (ids.size() == 1 || pos > last));
    last = pos;
    bool failed = false, running = false, held = false;
    for (const auto& s : table.sessions) {
      if (s.workspace != item.id) continue;
      held = true;
      failed = failed || (s.command == holder::CommandState::Done && s.exitCode != 0);
      running = running || s.command == holder::CommandState::Running;
    }
    CHECK(item.status == (failed ? Status::Failed : running ? Status::Running : Status::None));
    const Location want = item.id == here ? Location::Here : elsewhere.contains(item.id) ? Location::OtherWindow
                          : held ? Location::Detached : Location::Saved;
    CHECK(item.location == want);
  }
}

} // namespace

int main() {
  for (seed = 1; seed <= 2000; ++seed) {
    std::mt19937 rng(seed);
    auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
    holder::Workspaces table;
    const int count = pick(9);
    for (int i = 0; i < count; ++i) table.workspaces.push_back({.id = static_cast<std::uint64_t>(i * 3 + 1 + pick(3)), .name = "w"});
    for (int i = pick(20); i > 0; --i) {
      const auto command = static_cast<holder::CommandState>(pick(3));
      table.sessions.push_back({.id = static_cast<std::uint64_t>(i), .workspace = static_cast<std::uint64_t>(pick(30)),
                                .command = command, .exitCode = pick(3) == 0 ? 1 : 0});
    }
    std::set<std::uint64_t> elsewhere;
    for (int i = pick(4); i > 0; --i) elsewhere.insert(static_cast<std::uint64_t>(pick(30)));
    checkBar(table, static_cast<std::uint64_t>(pick(30)), elsewhere);
  }
  seed = 0;
  CHECK(tooltip({.name = "ci-runner", .location = Location::Detached, .failed = 1, .running = 2}) == "ci-runner · No window · 1 failed");
  if (failures == 0) std::puts("workspace_bar: all passed");
  return failures == 0 ? 0 : 1;
}
