#include "a11y/a11y_roots.h"

namespace a11y {
  void addRoot(const Node* /*root*/, std::function<std::string()> /*title*/) {}
  void removeRoot(const Node* /*root*/) {}
  void markDirty() noexcept {}
  bool active() noexcept { return false; }
} // namespace a11y
