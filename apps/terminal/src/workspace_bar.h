#pragma once

#include "holder_client.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The sidebar foot bar: one item per open window of the app, each window's workspace being its entry in the holder's
// workspace table.
namespace term::workspace_bar {

  enum class Status : std::uint8_t { None, Running, Failed };

  struct Window {
    std::uint64_t id = 0;                    // TerminalWindow::windowId
    std::optional<std::uint64_t> workspace; // nullopt until the holder answers its PutWorkspace
  };

  struct Item {
    std::uint64_t window = 0;
    std::string name;
    std::string symbol;
    bool here = false;            // the window the bar is in
    Status status = Status::None; // Failed wins over Running
    int failed = 0;               // sessions whose last command exited non-zero
    int running = 0;              // sessions running a command
    bool operator==(const Item&) const = default;
  };

  // In window order; empty with fewer than 2 windows.
  std::vector<Item> barFor(const holder::Workspaces& table, const std::vector<Window>& windows, std::uint64_t here);

  // "name · 1 failed"
  std::string tooltip(const Item& item);

} // namespace term::workspace_bar
