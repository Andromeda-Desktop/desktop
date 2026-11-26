#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Surface;

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

// A drag this app starts from a pointer press, offered to every drop target (its own surfaces too).
struct DragSource {
  std::vector<std::string> mimeTypes;
  std::function<std::string(const std::string& mime)> data; // bytes for one offered type
  std::uint32_t actions = DropCopy | DropMove;
  Surface* icon = nullptr; // the drag image (Display::createDragIcon), its top left at the pointer; the caller owns it
  // The action the target took and whether it dropped; called once.
  std::function<void(std::uint32_t action, bool dropped)> finished;
};
