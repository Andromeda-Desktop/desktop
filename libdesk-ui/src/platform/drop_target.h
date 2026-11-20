#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

enum DropAction : std::uint32_t { DropNone = 0, DropCopy = 1, DropMove = 2, DropAsk = 4 };

// A surface that takes drops from other apps.
struct DropTarget {
  // Pointer entered with these types; return false to refuse every drop on this surface.
  std::function<bool(const std::vector<std::string>& mimes)> enter;
  // Surface-local position; returns the DropAction a drop here would take (DropNone = not here).
  std::function<std::uint32_t(float x, float y)> motion;
  std::function<void()> leave;
  // The dropped data of the first accepted type, read off the main thread and delivered on it.
  std::function<void(float x, float y, std::uint32_t action, const std::string& mime, std::string data)> drop;
};
