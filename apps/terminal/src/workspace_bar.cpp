#include "workspace_bar.h"

#include <algorithm>
#include <format>

namespace term::workspace_bar {

  std::vector<Item> barFor(const holder::Workspaces& table, const std::vector<Window>& windows, std::uint64_t here) {
    std::vector<Item> items;
    if (windows.size() < 2) return items;
    for (const Window& w : windows) {
      // The mark is the window's place in the bar, 1 to 9: no two windows share one.
      const std::size_t place = items.size() + 1;
      Item item{.window = w.id, .name = w.name, .symbol = place <= 9 ? std::format("square-rounded-number-{}", place) : "folder",
                .here = w.id == here};
      if (w.workspace) {
        if (const auto it = std::ranges::find(table.workspaces, *w.workspace, &holder::WorkspaceInfo::id);
            it != table.workspaces.end() && !it->symbol.empty()) {
          item.symbol = it->symbol;
        }
        for (const holder::SessionInfo& s : table.sessions) {
          if (s.workspace != *w.workspace) continue;
          if (s.command == holder::CommandState::Running) ++item.running;
          if (s.command == holder::CommandState::Done && s.exitCode != 0) ++item.failed;
        }
      }
      item.status = item.failed > 0 ? Status::Failed : item.running > 0 ? Status::Running : Status::None;
      items.push_back(std::move(item));
    }
    return items;
  }

  std::string tooltip(const Item& item) {
    if (item.failed > 0) return std::format("{} · {} failed", item.name, item.failed);
    if (item.running > 0) return std::format("{} · {} running", item.name, item.running);
    return item.name;
  }

} // namespace term::workspace_bar
