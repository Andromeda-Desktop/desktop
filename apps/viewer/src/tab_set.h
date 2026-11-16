#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace viewer {

  namespace tab_event {
    // Adds `id` after the focused tab and focuses it.
    struct Open {
      std::uint64_t id;
    };
    // Removes `id`; focus moves to another pane of its split, else to the tab before it.
    struct Close {
      std::uint64_t id;
    };
    // Focuses `id`; a split member shows its whole split.
    struct Focus {
      std::uint64_t id;
    };
    // Puts `joining` beside `target` (before it when `before`) in target's split, and focuses it.
    struct Join {
      std::uint64_t target;
      std::uint64_t joining;
      bool before = false;
    };
    struct Leave {
      std::uint64_t id;
    };
  } // namespace tab_event

  using TabEvent = std::variant<tab_event::Open, tab_event::Close, tab_event::Focus, tab_event::Join, tab_event::Leave>;

  inline constexpr std::size_t kMaxPanes = 4;

  // The documents of one window: their order, the focused one and the splits.
  // Invariants: ids are distinct; focused() is set exactly when tabs() is not empty; a split holds 2..kMaxPanes tabs in
  // layout order; a tab is in at most one split.
  class TabSet {
  public:
    // False when the event does not apply (an unknown id, a full split, a member joining again); nothing changes.
    bool apply(const TabEvent& event);

    [[nodiscard]] const std::vector<std::uint64_t>& tabs() const { return m_tabs; }
    [[nodiscard]] std::optional<std::uint64_t> focused() const { return m_focused; }
    [[nodiscard]] const std::vector<std::vector<std::uint64_t>>& splits() const { return m_splits; }
    [[nodiscard]] const std::vector<std::uint64_t>* splitOf(std::uint64_t id) const;
    // The panes on screen in layout order: the focused tab's split, or the focused tab alone.
    [[nodiscard]] std::vector<std::uint64_t> visible() const;
    [[nodiscard]] bool contains(std::uint64_t id) const;

  private:
    void removeFromSplit(std::uint64_t id);

    std::vector<std::uint64_t> m_tabs;
    std::optional<std::uint64_t> m_focused;
    std::vector<std::vector<std::uint64_t>> m_splits;
  };

} // namespace viewer
