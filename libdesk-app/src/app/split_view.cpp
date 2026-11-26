#include "app/split_view.h"

#include "app/app_window.h"
#include "core/deferred_call.h"
#include "render/animation/animation_manager.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include "core/input/cursor_shape.h"

#include "core/input/input_codes.h"

#include <algorithm>
#include <cmath>

using TileNode = andromeda::tiling::DwindleTree::Node;

SplitView::SplitView(AppWindow& window, Callbacks callbacks) : m_window(window), m_callbacks(std::move(callbacks)) {
  // A press anywhere on a card (caption, content, a gap between pages) focuses it; the press still reaches its area.
  // Only a press whose area is in the card counts: an overlay over the card (a sheet, a menu) keeps its press.
  m_window.input().setPressCallback([this](float x, float y) {
    const Node* hit = m_window.input().inputAreaAt(x, y);
    for (const Card& card : m_cards) {
      bool inside = false;
      for (const Node* node = hit; node != nullptr && !inside; node = node->parent()) {
        inside = node == card.cell;
      }
      if (card.id != m_ringPane && inside) {
        m_callbacks.focus(card.id);
        return;
      }
    }
  });
}

SplitView::~SplitView() { m_window.input().setPressCallback({}); }

void SplitView::reset() {
  m_slot = nullptr;
  m_built.clear();
  m_cards.clear();
  m_dividers.clear();
  m_dragDivider.reset();
  m_dropPreview = nullptr;
  m_ringPane = 0;
}

void SplitView::fill(Renderer& renderer, Node* slot, float width, float height, const std::vector<std::uint64_t>& panes,
                     std::uint64_t focused, bool cards) {
  ++m_fills;
  std::vector<std::pair<std::uint64_t, PaneRect>> from;
  for (const Card& card : m_cards) {
    from.emplace_back(card.id, card.rect);
  }
  // The old single item counts as a card over the whole slot.
  if (from.empty() && m_built.size() == 1) {
    from.emplace_back(m_built.front(), PaneRect{0.0F, 0.0F, m_width, m_height});
  }
  m_slot = slot;
  m_width = width;
  m_height = height;
  m_cards.clear();
  m_dividers.clear();
  m_dragDivider.reset();
  m_dropPreview = nullptr;
  m_ringPane = 0;
  m_built.clear();
  if (panes.empty()) {
    return;
  }
  if (panes.size() > 1 && cards) {
    m_built = panes;
    buildCards(renderer, panes, from);
    setFocused(focused, false);
  } else {
    m_built = {focused};
    auto content = m_callbacks.content(renderer, focused, width, height);
    const auto old = std::ranges::find(from, focused, &std::pair<std::uint64_t, PaneRect>::first);
    if (from.size() > 1 && old != from.end()) {
      revealFrom(renderer, std::move(content), old->second, from);
    } else {
      m_slot->addChild(std::move(content));
    }
  }
  const float s = m_window.uiScale();
  m_slot->addChild(ui::box({.out = &m_dropPreview,
                            .fill = colorSpecFromRole(ColorRole::Accent, Tokens::componentSplitDropFill),
                            .border = focusRingColorSpec(),
                            .borderWidth = Tokens::componentSplitFocusRing * s,
                            .radius = Style::scaledRadius(Tokens::componentWindowCardRadius, s),
                            .visible = false,
                            .participatesInLayout = false}));
  if (const auto closed = std::exchange(m_closedCard, std::nullopt); closed) {
    const PaneRect& r = closed->rect;
    Box* plate = nullptr;
    auto box = ui::box({.out = &plate, .fill = closed->fill, .border = colorSpecFromRole(ColorRole::Stroke),
                        .borderWidth = Tokens::componentSplitStroke * s, .radius = Style::scaledRadius(Tokens::componentWindowCardRadius, s),
                        .width = r.width, .height = r.height, .participatesInLayout = false});
    box->setPosition(r.x, r.y);
    m_slot->insertChildAt(0, std::move(box));
    whenAnimating([this, plate](AnimationManager& animations) {
      animations.drive(0, 1.0F, 0.0F, Style::spring(Tokens::springFadeOut), [plate](float v) { plate->setOpacity(v); },
                       [this, plate] {
                         m_slot->removeChild(plate);
                         m_window.invalidate();
                       },
                       plate);
    });
  }
}

void SplitView::buildCards(Renderer& renderer, const std::vector<std::uint64_t>& panes,
                           const std::vector<std::pair<std::uint64_t, PaneRect>>& from) {
  const float s = m_window.uiScale();
  const float radius = Style::scaledRadius(Tokens::componentWindowCardRadius, s);
  const float caption = Tokens::componentWindowCaption * s;
  m_cards.reserve(panes.size()); // cells capture card pointers
  const std::vector<PaneRect> rects = paneRects(panes);
  float right = 0.0F;
  for (const PaneRect& r : rects) {
    right = std::max(right, r.x + r.width);
  }
  const ColorSpec clear = clearColorSpec();
  std::vector<std::unique_ptr<Node>> cells;
  for (std::size_t i = 0; i < panes.size() && i < rects.size(); ++i) {
    Card& card = m_cards.emplace_back(Card{.id = panes[i], .rect = rects[i], .fill = m_callbacks.surface(panes[i])});
    const PaneRect& r = card.rect;
    auto cell = ui::column({.out = &card.cell, .align = FlexAlign::Stretch, .minWidth = r.width, .minHeight = r.height,
                            .maxWidth = r.width, .maxHeight = r.height, .width = r.width, .height = r.height});
    cell->setPosition(r.x, r.y);
    auto shadow = ui::box({.out = &card.shadow,
                           .fill = colorSpecFromRole(ColorRole::Shadow, isResolvedLightTheme() ? Tokens::elevationE2.keyLight
                                                                                               : Tokens::elevationE2.keyDark),
                           .radius = radius,
                           .softness = Tokens::elevationE2.keyBlur * s,
                           .width = r.width,
                           .height = r.height,
                           .participatesInLayout = false});
    shadow->setPosition(0.0F, Tokens::elevationE2.keyY * s);
    // Cut out under the card, so a see-through card (a theme scene, glass) shows no dark tint.
    RoundedRectStyle cut = card.shadow->style();
    cut.shadowExclusion = true;
    cut.shadowExclusionOffsetY = Tokens::elevationE2.keyY * s;
    cut.shadowExclusionWidth = r.width;
    cut.shadowExclusionHeight = r.height;
    cut.shadowExclusionRadius = Radii{radius, radius, radius, radius};
    card.shadow->setStyle(cut);
    cell->addChild(std::move(shadow));
    auto box = ui::column({.align = FlexAlign::Stretch, .fill = card.fill, .radius = radius, .flexGrow = 1.0F});
    box->setClipRadii(Radii{radius, radius, radius, radius});
    // The caption's ink follows the card's plane, not the desktop scheme (as the window header does).
    box->setPaletteTone(toneOn(resolveColorSpec(card.fill)));

    // Caption (split mockup .cap): h 40, p 0 8 0 16; lead, name, state, accessory, trail, window buttons top right.
    auto cap = ui::row({.out = &card.caption, .align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .minWidth = r.width,
                        .minHeight = caption, .maxWidth = r.width, .maxHeight = caption, .width = r.width, .height = caption});
    cap->setPadding(0.0F, Tokens::spaceP8 * s, 0.0F, Tokens::spaceP16 * s);
    // A drag moves the window, as the header does. The area holds the caption row: a hit on
    // its labels finds the area among their parents.
    auto area = ui::inputArea({.width = r.width, .height = caption});
    card.captionArea = area.get();
    struct Press {
      bool down = false;
      bool handle = false; // on the lead or the name: the press drags the pane
      std::uint32_t serial = 0;
      float x = 0.0F;
      float y = 0.0F;
    };
    auto press = std::make_shared<Press>();
    area->setOnPress([this, press, id = card.id](const InputArea::PointerData& data) {
      if (data.button != BTN_LEFT) {
        return;
      }
      // By id: m_cards grows while the cards build.
      const auto card = std::ranges::find(m_cards, id, &Card::id);
      const auto on = [&](const Node* node) { return node != nullptr && node->containsScenePoint(data.sceneX, data.sceneY); };
      const bool handle = m_callbacks.drag && card != m_cards.end() && (on(card->lead) || on(card->name));
      *press = {.down = data.pressed, .handle = handle, .serial = data.serial,
                .x = data.localX, .y = data.localY};
      if (!data.pressed && m_dragPane == id) {
        m_dragPane.reset();
        m_callbacks.drag(id, Callbacks::DragPhase::Drop, data.sceneX, data.sceneY);
      }
    });
    area->setOnMotion([this, press, s, id = card.id](const InputArea::PointerData& data) {
      if (press->down && std::hypot(data.localX - press->x, data.localY - press->y) > 4.0F * s) {
        press->down = false;
        if (!press->handle) {
          m_window.beginMove(press->serial);
          return;
        }
        m_dragPane = id;
      }
      if (m_dragPane == id) {
        m_callbacks.drag(id, Callbacks::DragPhase::Move, data.sceneX, data.sceneY);
      }
    });
    Caption parts = m_callbacks.caption(card.id);
    // The window's header is gone in a split: the top left card holds its sidebar button.
    if (r.x == 0.0F && r.y == 0.0F) {
      if (auto toggle = m_window.buildSidebarToggle()) {
        cap->addChild(std::move(toggle));
      }
    }
    if (parts.lead) {
      card.lead = parts.lead.get();
      cap->addChild(std::move(parts.lead));
    }
    cap->addChild(ui::label({.out = &card.name,
                             .fontSize = Tokens::typeBody.size * s,
                             .color = colorSpecFromRole(ColorRole::Label1),
                             .minWidth = Tokens::spaceP24 * s,
                             .maxLines = 1,
                             .ellipsize = TextEllipsize::End}));
    card.name->setLineHeight(Tokens::typeBody.line * s);
    cap->addChild(ui::label({.out = &card.state,
                             .fontSize = Tokens::typeFootnote.size * s,
                             .maxLines = 1,
                             .ellipsize = TextEllipsize::End,
                             .visible = false,
                             .participatesInLayout = false}));
    card.state->setLineHeight(Tokens::typeFootnote.line * s);
    cap->addChild(ui::row({.out = &card.accessory, .align = FlexAlign::Center, .flexGrow = 1.0F}));
    if (parts.trail) {
      card.trail = parts.trail.get();
      cap->addChild(std::move(parts.trail));
    }
    if (r.y == 0.0F && r.x + r.width == right) {
      auto buttons = m_window.buildWindowButtons();
      card.buttons = buttons.get();
      cap->addChild(std::move(buttons));
    }
    card.captionArea->addChild(std::move(cap));
    box->addChild(std::move(area));
    auto holder = ui::column({.out = &card.holder, .align = FlexAlign::Stretch, .clipChildren = true, .flexGrow = 1.0F});
    holder->addChild(m_callbacks.content(renderer, card.id, r.width, std::max(0.0F, r.height - caption)));
    box->addChild(std::move(holder));
    cell->addChild(std::move(box));
    // Over the card: the contents fill their whole rect and would cover a border of the card itself. Clicks pass through.
    cell->addChild(ui::box({.out = &card.stroke, .fill = clear, .border = colorSpecFromRole(ColorRole::Stroke),
                            .borderWidth = Tokens::componentSplitStroke * s, .radius = radius, .width = r.width, .height = r.height,
                            .participatesInLayout = false}));
    cell->addChild(ui::box({.out = &card.ring, .fill = clear, .border = focusRingColorSpec(),
                            .borderWidth = Tokens::componentSplitFocusRing * s, .radius = radius, .width = r.width,
                            .height = r.height, .opacity = 0.0F, .participatesInLayout = false}));
    card.stroke->setHitTestVisible(false);
    card.ring->setHitTestVisible(false);
    cells.push_back(std::move(cell));
  }
  // A plain node lays its cards out where the tree put them.
  auto tiles = std::make_unique<Node>();
  for (std::unique_ptr<Node>& cell : cells) {
    tiles->addChild(std::move(cell));
  }
  m_slot->addChild(std::move(tiles));
  // Over each gap between cards: drag to resize both sides.
  const float grip = Tokens::spaceP32 * s;
  std::vector<TileNode*> splits;
  if (andromeda::tiling::DwindleTree* tree = syncTrees(panes); tree != nullptr) {
    std::vector<TileNode*> stack{tree->root()};
    while (!stack.empty()) {
      TileNode* node = stack.back();
      stack.pop_back();
      if (node != nullptr && node->type != TileNode::Leaf) {
        splits.push_back(node);
        stack.push_back(node->right.get());
        stack.push_back(node->left.get());
      }
    }
  }
  for (TileNode* split : splits) {
    const bool vertical = split->type == TileNode::HSplit;
    const std::size_t index = m_dividers.size();
    Divider& divider = m_dividers.emplace_back(Divider{.vertical = vertical, .split = split});
    auto area = ui::inputArea({.out = &divider.area,
                               .cursorShape = vertical ? CursorShape::EwResize
                                                       : CursorShape::NsResize,
                               .participatesInLayout = false});
    // Mockup .grip: a 4 x 32 capsule centred in the gap while the pointer is over it or drags it.
    auto handle = ui::box({.out = &divider.grip, .fill = colorSpecFromRole(ColorRole::Label3),
                           .radius = Tokens::spaceP4 * s / 2.0F, .width = vertical ? Tokens::spaceP4 * s : grip,
                           .height = vertical ? grip : Tokens::spaceP4 * s, .visible = false, .participatesInLayout = false});
    area->addChild(std::move(handle));
    area->setOnEnter([this, index](const InputArea::PointerData& /*data*/) {
      m_dividers[index].grip->setVisible(true);
      m_window.invalidate();
    });
    area->setOnLeave([this, index] {
      m_dividers[index].grip->setVisible(m_dragDivider == index);
      m_window.invalidate();
    });
    area->setOnPress([this, index](const InputArea::PointerData& data) {
      if (data.button != BTN_LEFT) {
        return;
      }
      if (data.pressed) {
        m_dragDivider = index;
        return;
      }
      m_dragDivider.reset();
      m_dividers[index].grip->setVisible(m_dividers[index].area->hovered());
      relayout(true);
      if (m_callbacks.treeChanged) m_callbacks.treeChanged();
    });
    area->setOnMotion([this, index](const InputArea::PointerData& data) {
      if (m_dragDivider == index) {
        dragDivider(index, data.sceneX, data.sceneY);
      }
    });
    m_slot->addChild(std::move(area));
  }
  placeDividers();
  // Motion: each card from its old place; a new card from the drop preview, else it fades in.
  std::vector<std::optional<PaneRect>> starts;
  bool moves = false;
  for (Card& card : m_cards) {
    std::optional<PaneRect> start;
    if (m_dropFrom && m_dropFrom->first == card.id) {
      start = m_dropFrom->second;
    } else if (const auto it = std::ranges::find(from, card.id, &std::pair<std::uint64_t, PaneRect>::first); it != from.end()) {
      start = it->second;
    }
    moves = moves || !start || start->x != card.rect.x || start->y != card.rect.y || start->width != card.rect.width ||
            start->height != card.rect.height;
    starts.push_back(start);
  }
  m_dropFrom.reset();
  if (!moves || from.empty()) {
    return;
  }
  const auto step = [this, starts](float t) {
    std::vector<PaneRect> rects;
    for (std::size_t i = 0; i < m_cards.size(); ++i) {
      const PaneRect& to = m_cards[i].rect;
      const PaneRect a = starts[i].value_or(to);
      const auto mix = [t](float x, float y) { return x + (y - x) * t; };
      rects.push_back({mix(a.x, to.x), mix(a.y, to.y), mix(a.width, to.width), mix(a.height, to.height)});
      if (!starts[i]) {
        // Content waits for content_gate of the travel, then fades in with it.
        m_cards[i].cell->setOpacity(std::clamp((t - Tokens::motionContentGate) / (1.0F - Tokens::motionContentGate), 0.0F, 1.0F));
      }
    }
    applyCellRects(rects);
    m_slot->markLayoutDirty();
  };
  step(0.0F);
  whenAnimating([this, step](AnimationManager& animations) {
    m_layoutAnim = animations.drive(m_layoutAnim, 0.0F, 1.0F, Style::spring(Tokens::springMove), step,
                                    [this] { m_window.invalidate(); }, m_cards.front().cell, 0.0F, Unit::Progress, true);
  });
}

void SplitView::revealFrom(Renderer& renderer, std::unique_ptr<Node> content, const PaneRect& start,
                           const std::vector<std::pair<std::uint64_t, PaneRect>>& from) {
  const float s = m_window.uiScale();
  const float radius = Style::scaledRadius(Tokens::componentWindowCardRadius, s);
  // The cards that leave fade out where they were, under the remaining one.
  for (const auto& [id, r] : from) {
    // A closed card has its own plate (fill).
    const bool closed = m_closedCard && m_closedCard->rect.x == r.x && m_closedCard->rect.y == r.y;
    if (id == m_built.front() || closed) {
      continue;
    }
    Box* plate = nullptr;
    auto box = ui::box({.out = &plate, .fill = m_callbacks.surface(id), .border = colorSpecFromRole(ColorRole::Stroke),
                        .borderWidth = Tokens::componentSplitStroke * s, .radius = radius, .width = r.width, .height = r.height,
                        .participatesInLayout = false});
    box->setPosition(r.x, r.y);
    m_slot->addChild(std::move(box));
    whenAnimating([this, plate](AnimationManager& animations) {
      animations.drive(0, 1.0F, 0.0F, Style::spring(Tokens::springFadeOut), [plate](float v) { plate->setOpacity(v); },
                       [this, plate] {
                         m_slot->removeChild(plate);
                         m_window.invalidate();
                       },
                       plate);
    });
  }
  // The remaining content lays out at its final size; a clip grows from its old card to the whole slot over it.
  content->setSize(m_width, m_height);
  content->layout(renderer);
  auto window = std::make_unique<Node>();
  window->setParticipatesInLayout(false);
  window->setClipChildren(true);
  Node* clip = window.get();
  Node* inner = clip->addChild(std::move(content));
  m_slot->addChild(std::move(window));
  const auto step = [clip, inner, start, w = m_width, h = m_height](float t) {
    const auto mix = [t](float a, float b) { return a + (b - a) * t; };
    const float x = mix(start.x, 0.0F);
    const float y = mix(start.y, 0.0F);
    clip->setPosition(x, y);
    clip->setSize(mix(start.width, w), mix(start.height, h));
    inner->setPosition(-x, -y);
  };
  step(0.0F);
  // At rest the content joins the slot's layout again, so a later resize sizes it.
  const auto settle = [this, clip, inner] {
    if (m_slot == nullptr || clip->parent() != m_slot) {
      return;
    }
    std::unique_ptr<Node> owned = clip->removeChild(inner);
    owned->setPosition(0.0F, 0.0F);
    m_slot->insertChildAt(0, std::move(owned));
    m_slot->removeChild(clip);
    m_slot->markLayoutDirty();
    m_window.invalidate();
  };
  whenAnimating([this, step, settle, clip](AnimationManager& animations) {
    m_layoutAnim = animations.drive(m_layoutAnim, 0.0F, 1.0F, Style::spring(Tokens::springMove),
                                    [this, step](float t) {
                                      step(t);
                                      m_window.invalidate();
                                    },
                                    settle, clip, 0.0F, Unit::Progress, true);
  });
}

void SplitView::whenAnimating(std::function<void(AnimationManager&)> start) {
  if (AnimationManager* animations = m_slot->animationManager(); animations != nullptr) {
    start(*animations);
    return;
  }
  DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive), fills = m_fills, start = std::move(start)] {
    if (alive.expired() || fills != m_fills || m_slot == nullptr) {
      return;
    }
    if (AnimationManager* animations = m_slot->animationManager(); animations != nullptr) {
      start(*animations);
      m_window.invalidate();
    }
  });
}

ui::SplitTrees::Room SplitView::room() const {
  return {.area = {.width = static_cast<int>(std::lround(m_width)), .height = static_cast<int>(std::lround(m_height))},
          .gap = static_cast<int>(std::lround(Tokens::componentWindowCardGap * m_window.uiScale())),
          .minWidth = m_callbacks.minimum().first};
}

std::vector<SplitView::PaneRect> SplitView::paneRects(const std::vector<std::uint64_t>& panes) {
  andromeda::tiling::DwindleTree* tree = syncTrees(panes);
  if (tree == nullptr) {
    return std::vector<PaneRect>(panes.size());
  }
  return ui::arrangePanes(*tree, panes,
                          {.width = static_cast<int>(std::lround(m_width)), .height = static_cast<int>(std::lround(m_height))},
                          static_cast<int>(std::lround(Tokens::componentWindowCardGap * m_window.uiScale())));
}

void SplitView::placeDividers() {
  if (m_cards.size() < 2) {
    return;
  }
  const float s = m_window.uiScale();
  const float gap = Tokens::componentWindowCardGap * s;
  const float hit = Tokens::componentSplitDividerHit * s;
  for (const Divider& divider : m_dividers) {
    const PaneRect r = ui::dividerRect(*divider.split, divider.vertical, gap, hit);
    divider.area->setPosition(r.x, r.y);
    divider.area->setSize(r.width, r.height);
    divider.grip->setPosition((r.width - divider.grip->width()) / 2.0F, (r.height - divider.grip->height()) / 2.0F);
  }
}

void SplitView::dragDivider(std::size_t index, float sceneX, float sceneY) {
  if (m_slot == nullptr || index >= m_dividers.size()) {
    return;
  }
  TileNode* split = m_dividers[index].split;
  const bool horizontal = m_dividers[index].vertical;
  float x = 0.0F;
  float y = 0.0F;
  Node::absolutePosition(m_slot, x, y);
  const float at = horizontal ? sceneX - x - static_cast<float>(split->area.x) : sceneY - y - static_cast<float>(split->area.y);
  const auto [minW, minH] = m_callbacks.minimum();
  split->ratio = ui::dividerRatio(*split, horizontal, at, Tokens::componentWindowCardGap * m_window.uiScale(), minW, minH);
  relayout(false);
}

bool SplitView::nudgeDivider(float dx, float dy) {
  constexpr double kStep = 0.05; // of the split's span
  const bool horizontal = dx != 0.0F;
  for (const andromeda::tiling::DwindleTree& tree : m_trees.trees()) {
    TileNode* node = tree.find(m_ringPane);
    for (TileNode* split = node != nullptr ? node->parent : nullptr; split != nullptr; split = split->parent) {
      if ((split->type == TileNode::HSplit) != horizontal) {
        continue;
      }
      const double span = horizontal ? split->area.width : split->area.height;
      const double to = (split->ratio + ((horizontal ? dx : dy) > 0.0F ? kStep : -kStep)) * span;
      const auto [minW, minH] = m_callbacks.minimum();
      split->ratio = ui::dividerRatio(*split, horizontal, static_cast<float>(to),
                                      Tokens::componentWindowCardGap * m_window.uiScale(), minW, minH);
      relayout(true);
      if (m_callbacks.treeChanged) m_callbacks.treeChanged();
      return true;
    }
  }
  return false;
}

void SplitView::swapPanes(std::uint64_t a, std::uint64_t b) {
  m_trees.swap(a, b);
  if (m_callbacks.treeChanged) m_callbacks.treeChanged();
}

andromeda::tiling::DwindleTree* SplitView::syncTrees(const std::vector<std::uint64_t>& panes) {
  const auto shapes = [this] {
    std::vector<std::vector<andromeda::tiling::DwindleTree::ShapeNode>> out;
    for (const andromeda::tiling::DwindleTree& tree : m_trees.trees()) out.push_back(tree.shape());
    return out;
  };
  const auto before = shapes();
  andromeda::tiling::DwindleTree* tree = m_trees.sync(m_callbacks.splitOf, panes, room());
  if (m_callbacks.treeChanged && shapes() != before) m_callbacks.treeChanged();
  return tree;
}

void SplitView::applyCellRects(const std::vector<PaneRect>& rects) {
  const float caption = Tokens::componentWindowCaption * m_window.uiScale();
  for (std::size_t i = 0; i < m_cards.size() && i < rects.size(); ++i) {
    Card& card = m_cards[i];
    const PaneRect& r = rects[i];
    card.cell->setMinWidth(r.width);
    card.cell->setMaxWidth(r.width);
    card.cell->setMinHeight(r.height);
    card.cell->setMaxHeight(r.height);
    card.cell->setPosition(r.x, r.y);
    card.cell->setSize(r.width, r.height);
    for (Box* box : {card.shadow, card.stroke, card.ring}) {
      box->setSize(r.width, r.height);
    }
    card.captionArea->setSize(r.width, caption);
    card.caption->setMinWidth(r.width);
    card.caption->setMaxWidth(r.width);
    card.caption->setSize(r.width, caption);
  }
}

void SplitView::relayout(bool settle) {
  if (m_cards.empty()) {
    return;
  }
  const std::vector<PaneRect> rects = paneRects(m_built);
  m_window.updateInPlace([this, &rects](Renderer& renderer) {
    for (std::size_t i = 0; i < m_cards.size() && i < rects.size(); ++i) {
      m_cards[i].rect = rects[i];
    }
    applyCellRects(rects);
    fitCaptions(renderer);
    placeDividers();
    m_slot->markLayoutDirty();
  });
  if (settle) {
    for (const auto& [id, r] : contentRects()) {
      m_callbacks.settle(id, r.width, r.height);
    }
  }
}

bool SplitView::rebuildContent(std::uint64_t id) {
  const auto it = std::ranges::find(m_cards, id, &Card::id);
  if (it == m_cards.end()) {
    return false;
  }
  const float caption = Tokens::componentWindowCaption * m_window.uiScale();
  m_window.updateInPlace([this, &card = *it, caption](Renderer& renderer) {
    while (!card.holder->children().empty()) {
      (void)card.holder->removeChild(card.holder->children().back().get());
    }
    card.holder->addChild(
        m_callbacks.content(renderer, card.id, card.rect.width, std::max(0.0F, card.rect.height - caption)));
    card.cell->layout(renderer);
  });
  return true;
}

std::optional<SplitView::PaneRect> SplitView::rect(std::uint64_t id) const {
  const auto it = std::ranges::find(m_cards, id, &Card::id);
  return it != m_cards.end() ? std::optional(it->rect) : std::nullopt;
}

std::vector<std::pair<std::uint64_t, SplitView::PaneRect>> SplitView::contentRects() const {
  const float caption = Tokens::componentWindowCaption * m_window.uiScale();
  std::vector<std::pair<std::uint64_t, PaneRect>> out;
  for (const Card& card : m_cards) {
    out.emplace_back(card.id, PaneRect{card.rect.x, card.rect.y + caption, card.rect.width,
                                       std::max(0.0F, card.rect.height - caption)});
  }
  return out;
}

void SplitView::paintFocus(bool animate) {
  // The header accessory is the focused item's: it leaves the old card before the new one builds it.
  std::vector<std::unique_ptr<Node>> removed;
  for (Card& card : m_cards) {
    while (!card.accessory->children().empty()) {
      removed.push_back(card.accessory->removeChild(card.accessory->children().back().get()));
    }
  }
  if (!removed.empty()) {
    m_window.input().forgetDetached();
  }
  const float ring = m_callbacks.ring() ? 1.0F : 0.0F;
  for (Card& card : m_cards) {
    const bool focused = card.id == m_ringPane;
    AnimationManager* animations = card.ring->animationManager();
    const float target = focused ? 1.0F : 0.0F;
    const auto set = [node = card.ring, shadow = card.shadow, ring](float v) {
      node->setOpacity(v * ring);
      shadow->setOpacity(Tokens::componentSplitRestShadow + v * (1.0F - Tokens::componentSplitRestShadow));
    };
    if (animate && animations != nullptr) {
      card.focusAnim = animations->drive(card.focusAnim, (card.shadow->opacity() - Tokens::componentSplitRestShadow) / (1.0F - Tokens::componentSplitRestShadow), target,
                                         Style::spring(focused ? Tokens::springFade : Tokens::springFadeOut), set, {},
                                         card.ring);
    } else {
      set(target);
    }
    card.name->setFontWeight(focused ? FontWeight::SemiBold : FontWeight::Normal);
    if (focused) {
      if (auto accessory = m_window.buildHeaderAccessory()) {
        card.accessory->addChild(std::move(accessory));
      }
    }
  }
}

void SplitView::setFocused(std::uint64_t id, bool animate) {
  if (m_cards.empty() || id == m_ringPane) {
    return;
  }
  m_ringPane = id;
  if (!animate) {
    paintFocus(false);
    return;
  }
  m_window.updateInPlace([this](Renderer& renderer) {
    paintFocus(true);
    fitCaptions(renderer);
    m_slot->markLayoutDirty();
  });
}

void SplitView::repaintFocus() {
  if (m_cards.empty()) {
    return;
  }
  for (Card& card : m_cards) {
    card.fill = m_callbacks.surface(card.id);
  }
  m_window.updateInPlace([this](Renderer& renderer) {
    paintFocus(false);
    fitCaptions(renderer);
    m_slot->markLayoutDirty();
  });
}

void SplitView::cancelPaneDrag() {
  if (const auto id = std::exchange(m_dragPane, std::nullopt); id && m_callbacks.drag) {
    m_callbacks.drag(*id, Callbacks::DragPhase::Cancel, 0.0F, 0.0F);
  }
}

bool SplitView::setCaption(std::uint64_t id, const std::string& name, const std::string& state, ColorSpec stateInk) {
  const auto it = std::ranges::find(m_cards, id, &Card::id);
  if (it == m_cards.end()) {
    return false;
  }
  const bool changed = it->name->text() != name || it->state->text() != state;
  it->name->setText(name);
  it->state->setText(state);
  it->state->setColor(stateInk);
  it->state->setVisible(!state.empty());
  it->state->setParticipatesInLayout(!state.empty());
  return changed;
}

// The state keeps its text (the name keeps its minimum); the name ellipsizes in what is left.
void SplitView::fitCaptions(Renderer& renderer) {
  const float s = m_window.uiScale();
  const float gap = Tokens::spaceP8 * s;
  const float nameMin = Tokens::spaceP24 * s;
  const auto width = [&](Node* node) {
    return node != nullptr && node->visible() && node->participatesInLayout()
        ? node->measure(renderer, LayoutConstraints{}).width + gap
        : 0.0F;
  };
  for (Card& card : m_cards) {
    // Padding 16 + 8, the gap before the accessory, then everything beside the name and the state.
    const float room = card.rect.width - (Tokens::spaceP16 + Tokens::spaceP8) * s - gap - width(card.lead) -
        width(card.trail) - width(card.buttons) -
        (card.accessory->children().empty() ? 0.0F : width(card.accessory));
    const float text = std::max(nameMin, room);
    card.state->setMaxWidth(std::max(nameMin, text - nameMin));
    card.name->setMaxWidth(std::max(nameMin, text - width(card.state)));
  }
}

std::optional<std::uint64_t> SplitView::paneInDirection(float dx, float dy) const {
  if (std::ranges::find(m_cards, m_ringPane, &Card::id) == m_cards.end()) {
    return std::nullopt;
  }
  ui::SplitPanes panes;
  for (const Card& card : m_cards) {
    panes.emplace_back(card.id, card.rect);
  }
  return ui::paneInDirection(panes, m_ringPane, dx, dy);
}

void SplitView::noteClosing(std::uint64_t id) {
  if (const auto it = std::ranges::find(m_cards, id, &Card::id); it != m_cards.end()) {
    m_closedCard = ClosedCard{it->rect, it->fill};
  }
}

std::optional<ui::SplitDrop> SplitView::dropAt(std::uint64_t shown, float sceneX, float sceneY) const {
  if (m_slot == nullptr) {
    return std::nullopt;
  }
  float x = 0.0F;
  float y = 0.0F;
  Node::absolutePosition(m_slot, x, y);
  std::vector<std::pair<std::uint64_t, PaneRect>> panes;
  if (m_cards.empty()) {
    panes.emplace_back(shown, PaneRect{0.0F, 0.0F, m_width, m_height});
  }
  for (const Card& card : m_cards) {
    panes.emplace_back(card.id, card.rect);
  }
  return ui::splitDrop(panes, sceneX - x, sceneY - y);
}

void SplitView::showDropPreview(const std::optional<PaneRect>& rect) {
  if (m_dropPreview == nullptr) {
    return;
  }
  if (rect) {
    m_dropPreview->setPosition(rect->x, rect->y);
    m_dropPreview->setSize(rect->width, rect->height);
  }
  m_dropPreview->setVisible(rect.has_value());
  m_window.invalidate();
}

void SplitView::pendJoin(const ui::SplitTrees::Join& join, std::optional<PaneRect> from) {
  m_trees.pendJoin(join);
  if (from) {
    m_dropFrom = std::pair{join.joining, *from};
  }
}
