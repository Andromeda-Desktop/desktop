#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class Node;
class TerminalGridNode;

namespace term {

// What one blank prompt row shows: cwd pill, branch pill, caption at the row end.
struct ContextItem {
  enum class Caption : std::uint8_t { None, Done, Failed, Running };
  std::string path;   // the cwd
  std::string cwd;    // as shown, ~ for $HOME
  std::string branch; // empty outside a git work tree
  Caption caption = Caption::None;
  std::string exit;   // Failed: "exit N"
  std::string time;   // "0.4 s", "3:08", "running · 0:12"
  bool operator==(const ContextItem&) const = default;
};

// The context rows of the visible prompts, in a grid layer below the text that moves with the grid origin.
class ContextRow {
public:
  struct Placed {
    std::uint32_t row; // viewport row
    ContextItem item;
  };

  struct Hit {
    enum class Pill : std::uint8_t { Cwd, Branch };
    Pill pill;
    std::string value; // Cwd: the path; Branch: the branch name
  };

  explicit ContextRow(TerminalGridNode& grid);
  void setOrigin(float x, float y);
  // Moves the nodes when only rows changed; rebuilds them when an item or a size changed.
  void sync(std::vector<Placed> placed, float cellHeight, float rowWidth, float scale);
  // The pill under (x, y) in grid coordinates.
  [[nodiscard]] std::optional<Hit> hitTest(float x, float y) const;

private:
  Node* m_layer = nullptr;
  std::vector<Placed> m_placed;
  std::vector<Node*> m_nodes;
  float m_cellHeight = 0.0F;
  float m_rowWidth = 0.0F;
  float m_scale = 0.0F;
};

} // namespace term
