#pragma once

#include <functional>
#include <string>

class Node;

// The platform accessibility bridge: AT-SPI on Linux, none elsewhere yet.
namespace a11y {
  void addRoot(const Node* root, std::function<std::string()> title);
  void removeRoot(const Node* root);
  void markDirty() noexcept;
} // namespace a11y
