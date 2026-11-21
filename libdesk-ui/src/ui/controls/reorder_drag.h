#pragma once

#include <cstddef>
#include <optional>
#include <vector>

// Pointer reorder of a vertical list, as pure state: rows lay out at fixed slots, the dragged row
// follows the pointer and the others shift to open a gap. Units are logical px.
class ReorderDrag {
public:
  enum class Phase { Idle, Armed, Dragging, Settling };

  struct Row {
    float top = 0.0F;
    float height = 0.0F;
  };

  struct Move {
    std::size_t from = 0;
    // Insertion index in [0, rows]: the item goes before the row at this index in the old order.
    std::size_t insertion = 0;
  };

  explicit ReorderDrag(float slop) : m_slop(slop) {}

  [[nodiscard]] Phase phase() const noexcept { return m_phase; }
  [[nodiscard]] std::optional<std::size_t> liftedRow() const noexcept;

  // Idle -> Armed. Ignored in any other phase or for an index outside rows.
  void press(std::size_t row, float pointerY, std::vector<Row> rows);
  // Armed -> Dragging past the slop; Dragging tracks the pointer.
  void motion(float pointerY);
  // Armed -> Idle; Dragging -> Settling.
  void release();
  // Settling -> Idle. Returns the move to commit, or nothing when the order does not change.
  std::optional<Move> settled();
  // Any phase -> Idle without a move (Escape, pointer capture lost, list rebuilt).
  void cancel();

  // Offset from its slot: the lifted row follows the pointer (clamped to the list); while Settling it
  // targets its new slot; other rows shift by the lifted row's height plus the gap to open the gap.
  [[nodiscard]] float offsetFor(std::size_t row) const;
  [[nodiscard]] std::size_t insertion() const noexcept { return m_insertion; }

private:
  [[nodiscard]] std::size_t insertionFor(float liftedCenter) const;
  [[nodiscard]] float liftedTravel() const;
  [[nodiscard]] float pitch() const;

  float m_slop;
  Phase m_phase = Phase::Idle;
  std::vector<Row> m_rows;
  std::size_t m_from = 0;
  std::size_t m_insertion = 0;
  float m_pressY = 0.0F;
  float m_pointerY = 0.0F;
};

// Applies a ReorderDrag::Move to a list; false when it changes nothing.
template <typename T> bool applyReorder(std::vector<T>& items, ReorderDrag::Move move) {
  if (move.from >= items.size() || move.insertion > items.size() || move.insertion == move.from
      || move.insertion == move.from + 1) {
    return false;
  }
  auto value = std::move(items[move.from]);
  items.erase(items.begin() + static_cast<std::ptrdiff_t>(move.from));
  const std::size_t to = move.insertion > move.from ? move.insertion - 1 : move.insertion;
  items.insert(items.begin() + static_cast<std::ptrdiff_t>(to), std::move(value));
  return true;
}
