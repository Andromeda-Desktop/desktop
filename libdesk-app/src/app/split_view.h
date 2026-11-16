#pragma once

#include "ui/palette.h"
#include "ui/split_layout.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class AnimationManager;
class AppWindow;
class Box;
class Flex;
class InputArea;
class Label;
class Node;
class Renderer;

// The content slot of a window that shows one item alone or a split of 2..4 items as cards (design-split): each card
// has a 40 caption (lead, name, state, the focused card's header accessory, trail; window buttons on the top-right
// card) over the item's content. Dividers resize panes; a drop preview shows where a dragged item joins; cards travel
// from their old rects on the move spring, a new card fades in, a closed card's plate fades out. The app owns which
// items show and which one has focus.
class SplitView {
public:
  using PaneRect = ui::PaneRect;
  struct Caption {
    std::unique_ptr<Node> lead;  // before the name (a symbol); may be null
    std::unique_ptr<Node> trail; // after the accessory (a menu button); may be null
  };
  struct Callbacks {
    ui::SplitTrees::SplitOf splitOf;
    // An item's content at this size (px); the slot's whole size while it shows alone.
    std::function<std::unique_ptr<Node>(Renderer&, std::uint64_t id, float width, float height)> content;
    std::function<Caption(std::uint64_t id)> caption;
    // The card fill of an item; the caption's ink follows it.
    std::function<ColorSpec(std::uint64_t id)> surface;
    // A caption click: the app focuses the item.
    std::function<void(std::uint64_t id)> focus;
    // The smallest pane (px), caption included.
    std::function<std::pair<float, float>()> minimum;
    // A pane's content took its settled size after a divider drag (the drag itself resizes only the cards).
    std::function<void(std::uint64_t id, float width, float height)> settle;
    // False: the focused card shows no ring (its shadow still lifts it).
    std::function<bool()> ring;
  };

  SplitView(AppWindow& window, Callbacks callbacks);
  ~SplitView();
  SplitView(const SplitView&) = delete;
  SplitView& operator=(const SplitView&) = delete;

  // Fills the empty `slot` (width x height px) with `panes` in layout order: cards when `cards` (the window has no
  // header), else the focused item's content alone. The old cards, or the old single item, are where the new ones
  // travel from.
  void fill(Renderer& renderer, Node* slot, float width, float height, const std::vector<std::uint64_t>& panes,
            std::uint64_t focused, bool cards);
  // The slot left the scene; the next fill starts without motion.
  void reset();

  [[nodiscard]] bool split() const { return !m_cards.empty(); }
  [[nodiscard]] bool dragging() const { return m_dragDivider.has_value(); }
  // The card that has focus; 0 while one item shows.
  [[nodiscard]] std::uint64_t focusedPane() const { return m_ringPane; }
  // The rect of a card in the slot (px).
  [[nodiscard]] std::optional<PaneRect> rect(std::uint64_t id) const;
  // Each card's item with its content size (caption excluded).
  [[nodiscard]] std::vector<std::pair<std::uint64_t, PaneRect>> contentRects() const;

  // Ring, shadow, caption weight and the header accessory move to `id`'s card in place; animate: they fade.
  void setFocused(std::uint64_t id, bool animate);
  // The focus look again (the ring setting or a fill changed).
  void repaintFocus();
  // A card's caption text; true when it changed (call fitCaptions).
  bool setCaption(std::uint64_t id, const std::string& name, const std::string& state, ColorSpec stateInk);
  // The name ellipsizes in the room the state, accessory, lead and trail leave.
  void fitCaptions(Renderer& renderer);
  // The card nearest in direction (dx, dy) from the focused one.
  [[nodiscard]] std::optional<std::uint64_t> paneInDirection(float dx, float dy) const;
  // The next fill fades a plate out where `id`'s card was.
  void noteClosing(std::uint64_t id);

  // Where `dragged` would join at the scene point: a half of a card, or of the single item `shown`.
  [[nodiscard]] std::optional<ui::SplitDrop> dropAt(std::uint64_t shown, float sceneX, float sceneY) const;
  // The drop preview at `rect` in the slot, or hidden.
  void showDropPreview(const std::optional<PaneRect>& rect);
  // The next fill puts `join.joining` on `join.edge` of `join.target`; from: its card starts at this rect.
  void pendJoin(const ui::SplitTrees::Join& join, std::optional<PaneRect> from = std::nullopt);

private:
  struct Card {
    std::uint64_t id = 0;
    PaneRect rect;
    ColorSpec fill;
    Flex* cell = nullptr;
    InputArea* captionArea = nullptr;
    Box* stroke = nullptr; // the card's edge, over its content
    Box* ring = nullptr;   // the focus ring over the stroke
    std::uint32_t focusAnim = 0;
    Box* shadow = nullptr;
    Node* lead = nullptr;
    Node* trail = nullptr;
    Node* buttons = nullptr;
    Label* name = nullptr;
    Label* state = nullptr;
    Flex* accessory = nullptr; // the window's header accessory while the card has focus
  };
  struct Divider {
    bool vertical = true; // between columns
    andromeda::tiling::DwindleTree::Node* split = nullptr;
    InputArea* area = nullptr;
    Box* grip = nullptr;
  };
  struct ClosedCard {
    PaneRect rect;
    ColorSpec fill;
  };

  void buildCards(Renderer& renderer, const std::vector<std::uint64_t>& panes,
                  const std::vector<std::pair<std::uint64_t, PaneRect>>& from);
  [[nodiscard]] ui::SplitTrees::Room room() const;
  [[nodiscard]] std::vector<PaneRect> paneRects(const std::vector<std::uint64_t>& panes);
  void placeDividers();
  void dragDivider(std::size_t index, float sceneX, float sceneY);
  // Cells, rings and caption areas at these rects; card.rect stays.
  void applyCellRects(const std::vector<PaneRect>& rects);
  // Cards follow the tree in place; settle: the contents take their new sizes.
  void relayout(bool settle);
  void paintFocus(bool animate);
  // Runs `start` with the slot's animation manager: now, or once this fill is in the scene.
  void whenAnimating(std::function<void(AnimationManager&)> start);

  AppWindow& m_window;
  Callbacks m_callbacks;
  Node* m_slot = nullptr;
  float m_width = 0.0F;
  float m_height = 0.0F;
  std::vector<std::uint64_t> m_built; // the items in the slot, in layout order
  std::vector<Card> m_cards;          // empty while one item shows
  ui::SplitTrees m_trees;
  std::vector<Divider> m_dividers;
  std::optional<std::size_t> m_dragDivider; // the divider under a held button
  Box* m_dropPreview = nullptr;
  std::optional<ClosedCard> m_closedCard;
  std::optional<std::pair<std::uint64_t, PaneRect>> m_dropFrom;
  std::uint64_t m_ringPane = 0;
  std::uint32_t m_layoutAnim = 0;
  std::uint64_t m_fills = 0;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
