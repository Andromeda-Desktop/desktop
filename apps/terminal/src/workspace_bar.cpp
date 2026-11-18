#include "workspace_bar.h"

#include <algorithm>
#include <format>

namespace term::workspace_bar {

  Bar barFor(const holder::Workspaces& table, std::uint64_t here, const std::set<std::uint64_t>& elsewhere) {
    Bar bar;
    if (table.workspaces.size() < 2) return bar;
    std::vector<Item> all;
    for (const holder::WorkspaceInfo& w : table.workspaces) {
      Item item{.id = w.id, .name = w.name, .symbol = w.symbol};
      bool held = false;
      for (const holder::SessionInfo& s : table.sessions) {
        if (s.workspace != w.id) continue;
        held = true;
        if (s.command == holder::CommandState::Running) ++item.running;
        if (s.command == holder::CommandState::Done && s.exitCode != 0) ++item.failed;
      }
      item.status = item.failed > 0 ? Status::Failed : item.running > 0 ? Status::Running : Status::None;
      item.location = w.id == here              ? Location::Here
                      : elsewhere.contains(w.id) ? Location::OtherWindow
                      : held                     ? Location::Detached
                                                 : Location::Saved;
      if (item.location != Location::OtherWindow) {
        if (item.location == Location::Here) bar.here = bar.pages.size();
        bar.pages.push_back(w.id);
      }
      all.push_back(std::move(item));
    }
    // A window without a workspace yet does not page.
    if (std::ranges::find(all, Location::Here, &Item::location) == all.end()) bar.pages.clear();
    if (all.size() <= kMaxItems) {
      bar.items = std::move(all);
      return bar;
    }
    const std::size_t shown = kMaxItems - 1;
    bar.items.assign(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(shown));
    if (const auto it = std::ranges::find(all, Location::Here, &Item::location);
        it != all.end() && std::ranges::find(bar.items, Location::Here, &Item::location) == bar.items.end()) {
      bar.items.back() = *it;
    }
    bar.more = all.size() - shown;
    return bar;
  }

  std::string tooltip(const Item& item) {
    static constexpr const char* kWhere[] = {"This window", "Other window", "No window", "Saved"};
    std::string out = std::format("{} · {}", item.name, kWhere[static_cast<std::size_t>(item.location)]);
    if (item.failed > 0) out += std::format(" · {} failed", item.failed);
    else if (item.running > 0) out += std::format(" · {} running", item.running);
    return out;
  }

} // namespace term::workspace_bar
