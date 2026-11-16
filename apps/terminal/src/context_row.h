#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class Node;
class Renderer;
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

// How long a command ran: "0.4 s", "18 s", "4:12".
[[nodiscard]] std::string formatTook(std::chrono::duration<double> took);

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

  // The grid's mono font: the band's path and branch read as part of the prompt line.
  struct Font {
    std::string family;
    float size = 0.0F; // px, ui scale applied
    bool operator==(const Font&) const = default;
  };

  explicit ContextRow(TerminalGridNode& grid);
  void setOrigin(float x, float y);
  // Keeps the rows for apply(); the caller asks for a frame.
  void sync(std::vector<Placed> placed, float cellHeight, float rowWidth, float scale, Font font, std::uint32_t accent);
  // Moves the nodes when only rows changed; rebuilds them when an item or a size changed. Runs in the frame tick, before
  // the window's layout pass and with GL current: rows added after that pass would draw unlaid.
  void apply(Renderer& renderer);
  // The path or branch under (x, y) in grid coordinates.
  [[nodiscard]] std::optional<Hit> hitTest(float x, float y) const;

private:
  TerminalGridNode& m_grid;
  Node* m_layer = nullptr;
  float m_backing = 0.0F;
  std::vector<Placed> m_placed;
  struct Input {
    std::vector<Placed> placed;
    float cellHeight = 0.0F;
    float rowWidth = 0.0F;
    float scale = 0.0F;
    Font font;
    std::uint32_t accent = 0; // 0xRRGGBB, the theme accent the cursor uses
  };
  std::optional<Input> m_pending; // set by sync(), taken by apply()
  std::vector<Node*> m_nodes;
  std::vector<Node*> m_marks; // the brand mark per placed row
  float m_cellHeight = 0.0F;
  float m_rowWidth = 0.0F;
  float m_scale = 0.0F;
  Font m_font;
  std::uint32_t m_accent = 0;
};

} // namespace term
