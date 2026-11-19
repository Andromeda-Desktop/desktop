#pragma once

#include "holder_client.h"

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

// The sidebar foot bar of terminal workspaces: what each window shows of the holder's workspace table.
namespace term::workspace_bar {

  enum class Location : std::uint8_t { Here, OtherWindow, Detached, Saved };
  enum class Status : std::uint8_t { None, Running, Failed };

  struct Item {
    std::uint64_t id = 0;
    std::string name;
    std::string symbol;
    Location location = Location::Saved;
    Status status = Status::None; // Failed wins over Running
    int failed = 0;               // sessions whose last command exited non-zero
    int running = 0;              // sessions running a command
    bool operator==(const Item&) const = default;
  };

  struct Bar {
    std::vector<Item> items; // empty with fewer than 2 workspaces
    std::size_t more = 0;    // workspaces not among items
    // The pager's pages in table order: the workspaces no other window shows. here: this window's index.
    std::vector<std::uint64_t> pages;
    std::size_t here = 0;
    bool operator==(const Bar&) const = default;
  };

  inline constexpr std::size_t kMaxItems = 4;

  // here: this window's workspace (0: none yet); elsewhere: workspaces other windows of the app show.
  Bar barFor(const holder::Workspaces& table, std::uint64_t here, const std::set<std::uint64_t>& elsewhere);

  // "name · No window · 1 failed"
  std::string tooltip(const Item& item);

} // namespace term::workspace_bar
