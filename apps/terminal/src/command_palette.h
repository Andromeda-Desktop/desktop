#pragma once

#include "palette_model.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class AnimationManager;
class Box;
class Flex;
class Input;
class InputArea;
class Node;

namespace term {

  // The command palette (Ctrl+Shift+P, design-palette B "Prompt line"): a prompt
  // line docked to the bottom of a pane and the results card above it, best match
  // next to the line. The root is out of its parent's layout: the owner calls
  // root->layout() after build() and after each relayout request, then
  // afterLayout().
  class CommandPalette {
  public:
    struct Host {
      std::function<void(const palette::Item&)> run;            // Enter or a click on a row that can run
      std::function<void(const std::string& query)> runNoMatch; // Enter while nothing matches
      std::function<void(const palette::Item*)> current;        // the current row changed; null: none
      std::function<void()> close;                              // Escape, or focus left the field
      std::function<void()> relayout;                           // the rows changed
    };
    struct Rect {
      float x = 0.0F;
      float y = 0.0F;
      float width = 0.0F;
      float height = 0.0F;
    };

    // recent: ids of the last picks, newest first.
    CommandPalette(AnimationManager& animations, std::vector<palette::Item> items, std::vector<std::string> recent,
                   Host host);

    // dim: the pane area under its caption; dock: the area whose bottom holds the
    // palette (inset p8).
    [[nodiscard]] std::unique_ptr<Node> build(float scale, float slotWidth, float slotHeight, Rect dim, Rect dock);
    [[nodiscard]] InputArea* focusArea() const;
    // Places the lens and the card shadows; with `animate` the lens travels after
    // an Up or Down.
    void afterLayout(bool animate);
    void grow();
    // `done` runs once the root may leave the scene.
    void shrink(std::function<void()> done);
    // The host callbacks stop: nodes still in the scene call nothing.
    void detach();

  private:
    void filter();
    void fillRows();
    void moveCurrent(int by);
    void setCurrent(std::size_t index);
    bool key(std::uint32_t sym, std::uint32_t mods);
    void pick(std::size_t index);
    [[nodiscard]] std::unique_ptr<Flex> makeRow(std::size_t index, bool current);

    AnimationManager& m_animations;
    std::vector<palette::Item> m_items;
    std::vector<std::string> m_recent;
    Host m_host;
    bool m_detached = false; // closed: host callbacks are no longer called
    std::string m_query;
    std::vector<palette::Match> m_list; // nearest the line first
    std::size_t m_recentCount = 0;      // empty field: m_list starts with this many
                                        // recent picks, then sessions
    std::size_t m_current = 0;
    std::size_t m_offset = 0; // the first shown entry of m_list
    float m_scale = 1.0F;
    float m_rowWidth = 0.0F;
    float m_lensTarget = 0.0F; // the current row's y in the results card
    float m_lensY = 0.0F;
    bool m_lensShown = false;
    bool m_travel = false; // the current row moved (Up, Down): the lens travels
                           // instead of jumping
    std::uint32_t m_lensAnim = 0;
    Node* m_root = nullptr;
    Flex* m_stack = nullptr;
    Flex* m_results = nullptr;
    Flex* m_rows = nullptr; // the results card's lines, under the lens
    Flex* m_line = nullptr;
    Box* m_lens = nullptr;
    Box* m_lensEdge = nullptr;
    Box* m_resultsShadow = nullptr;
    Box* m_lineShadow = nullptr;
    Input* m_input = nullptr;
  };

} // namespace term
