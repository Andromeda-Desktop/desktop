#include "a11y/a11y_roots.h"

namespace a11y {
  void addRoot(const Node* /*root*/, std::function<std::string()> /*title*/) {}
  void removeRoot(const Node* /*root*/) {}
  void markDirty() noexcept {}
} // namespace a11y
