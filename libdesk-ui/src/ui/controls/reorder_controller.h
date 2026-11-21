#pragma once

#include "ui/controls/flex.h"
#include "ui/controls/reorder_drag.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class Box;
class Button;
class Node;

// Drag reorder for the rows of a vertical list (rulebook: slop 8, lift to E3 at component.edit.drag_scale on snap,
// the other rows reflow on move). The row order changes only through `onMove`, once, after the lifted row settles;
// the owner then rebuilds its rows and calls attach() again.
class ReorderController {
public:
  ReorderController(float scale, std::function<void(ReorderDrag::Move)> onMove);
  ~ReorderController();

  ReorderController(const ReorderController&) = delete;
  ReorderController& operator=(const ReorderController&) = delete;

  // Applies while no drag runs; the owner rebuilds its rows after a scale change.
  void setScale(float scale);
  // `list` holds the rows; `decor` (separators) fades out while a row is lifted. Cancels a drag in progress.
  void attach(Node& list, std::vector<Node*> rows, std::vector<Node*> decor);
  // The grab handle of row `index`: drag it, or press Alt+Up / Alt+Down while it has focus. Escape cancels a drag.
  [[nodiscard]] std::unique_ptr<Button> makeHandle(std::size_t index, std::string_view tooltip);

private:
  void press(std::size_t index, const Node& handle, float localY);
  void motion(const Node& handle, float localY);
  void release();
  void cancel();
  void lift(bool lifted);
  void driveOffsets();
  void syncShadow();
  void removeLiftNodes();
  void restoreTooltips();
  void commitLater(ReorderDrag::Move move);
  [[nodiscard]] float listY(const Node& handle, float localY) const;

  float m_scale;
  std::function<void(ReorderDrag::Move)> m_onMove;
  ReorderDrag m_drag;
  Node* m_list = nullptr;
  std::vector<Node*> m_rows;
  std::vector<Node*> m_decor;
  std::vector<std::uint32_t> m_offsetAnims;
  std::vector<Button*> m_handles;
  std::vector<Button*> m_pendingHandles;
  std::string m_tooltip;
  // The lifted row draws on a solid plate over an E3 shadow: list rows have no fill of their own.
  Box* m_shadow = nullptr;
  Box* m_plate = nullptr;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

// A column whose rows reorder by drag: the controller lives exactly as long as the rows it moves.
class ReorderList : public Flex {
public:
  ReorderList(float scale, std::function<void(ReorderDrag::Move)> onMove) : m_reorder(scale, std::move(onMove)) {
    setDirection(FlexDirection::Vertical);
  }

  [[nodiscard]] ReorderController& reorder() noexcept { return m_reorder; }

private:
  ReorderController m_reorder;
};
