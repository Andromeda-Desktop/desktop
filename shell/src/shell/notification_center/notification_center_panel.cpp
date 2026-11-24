#include "shell/notification_center/notification_center_panel.h"

#include "compositors/compositor_platform.h"
#include "core/input/cursor_shape.h"
#include "i18n/i18n.h"
#include "notification/notification_display_name.h"
#include "notification/notification_manager.h"
#include "render/core/texture_manager.h"
#include "render/scene/input_area.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace nc = notification_center;

namespace {

  std::unique_ptr<Button> pill(std::string text, float scale, std::function<void()> onClick) {
    return ui::button({
        .text = std::move(text),
        .fontSize = Tokens::typeBody.size * scale,
        .controlHeight = Tokens::sizeControlM * scale,
        .variant = ButtonVariant::Default,
        .radius = Tokens::sizeControlM * 0.5F * scale,
        .onClick = std::move(onClick),
        .configure = [](Button& b) { b.label()->setFontWeight(static_cast<FontWeight>(Tokens::typeHeadline.weight)); },
    });
  }

  // Cards and the back cards of a stack: fill_2 with a hairline edge, so stacked cards read as separate cards on glass.
  void styleCard(Flex& card, float scale) {
    card.setFill(colorSpecFromRole(ColorRole::Fill2));
    card.setRadius(Style::scaledRadius(Tokens::radiusR16, scale));
    card.setBorder(colorSpecFromRole(ColorRole::Separator), scale);
  }

  std::unique_ptr<Button> plainPill(std::string text, float scale, std::function<void()> onClick,
                                    std::optional<std::string> glyph = std::nullopt) {
    return ui::button({
        .text = std::move(text),
        .glyph = std::move(glyph),
        .fontSize = Tokens::typeFootnote.size * scale,
        .glyphSize = Tokens::sizeSymbolInline * scale,
        .controlHeight = Tokens::sizeControlS * scale,
        .variant = ButtonVariant::Ghost,
        .radius = Tokens::sizeControlS * 0.5F * scale,
        .onClick = std::move(onClick),
    });
  }

} // namespace

float NotificationCenterPanel::preferredWidth() const { return scaled(Tokens::componentNotificationCenterWidth); }

void NotificationCenterPanel::onOpen(std::string_view /*context*/) {
  if (m_notifications == nullptr) {
    return;
  }
  m_notifications->markNotificationHistorySeen();
  // PanelManager calls onOpen again on the open panel for a new context: subscribe once per open.
  if (m_eventToken.has_value()) {
    return;
  }
  m_eventToken = m_notifications->addEventCallback([this](const Notification& /*n*/, NotificationEvent event) {
    if (event == NotificationEvent::Added) {
      m_notifications->markNotificationHistorySeen();
    }
    PanelManager::instance().refresh();
  });
}

void NotificationCenterPanel::onClose() {
  if (m_notifications != nullptr && m_eventToken.has_value()) {
    m_notifications->removeEventCallback(*m_eventToken);
  }
  m_eventToken.reset();
  m_openGroups.clear();
  m_column = nullptr;
  m_scroll = nullptr;
  m_list = nullptr;
  m_cards.clear();
  m_peeks.clear();
  m_moving.clear();
  m_icons.clear();
  m_key.clear();
  m_slotHeight = 0.0F;
}

void NotificationCenterPanel::create() {
  const float scale = contentScale();
  auto column = ui::column({.out = &m_column, .align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * scale});

  auto head = ui::row(
      {.align = FlexAlign::Center,
       .justify = FlexJustify::SpaceBetween,
       .minHeight = Tokens::sizeControlXl * scale,
       .fillWidth = true},
      ui::label({
          .text = i18n::tr("control-center.detail.notifications.title"),
          .fontSize = Tokens::typeHeadline.size * scale,
          .fontWeight = static_cast<FontWeight>(Tokens::typeHeadline.weight),
          .color = colorSpecFromRole(ColorRole::Label1),
      }),
      pill(i18n::tr("control-center.detail.notifications.clear-all"), scale, [this] {
        m_notifications->clearHistory();
        requestRebuild();
      })
  );
  head->setPadding(0.0F, 0.0F, 0.0F, Tokens::spaceP4 * scale);
  column->addChild(std::move(head));

  auto scroll = std::make_unique<ScrollView>();
  scroll->setOrientation(ScrollOrientation::Vertical);
  scroll->setContentScale(scale);
  scroll->setViewportPaddingH(0.0F);
  scroll->setViewportPaddingV(0.0F);
  scroll->setScrollbarVisible(false);
  scroll->clearFill();
  scroll->clearBorder();
  scroll->setFlexGrow(1.0F);
  m_scroll = scroll.get();
  m_list = scroll->content();
  m_list->setDirection(FlexDirection::Vertical);
  m_list->setAlign(FlexAlign::Stretch);
  m_list->setGap(Tokens::spaceP8 * scale);
  column->addChild(std::move(scroll));

  setRoot(std::move(column));
  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }
  rebuild();
}

std::string NotificationCenterPanel::contentKey() const {
  if (m_notifications == nullptr) {
    return {};
  }
  std::string key;
  for (const NotificationHistoryEntry& entry : m_notifications->history()) {
    key += std::format(
        "{}:{}:{};", entry.notification.id, nc::ageText(entry.notification), entry.notification.imageData.has_value()
    );
  }
  for (const std::string& group : m_openGroups) {
    key += "+" + group;
  }
  return key;
}

void NotificationCenterPanel::requestRebuild() {
  m_dirty = true;
  PanelManager::instance().refresh();
}

void NotificationCenterPanel::captureReflow() {
  m_reflowFrom.clear();
  for (const Moving& m : m_moving) {
    float x = 0.0F;
    float y = 0.0F;
    Node::absolutePosition(m.node, x, y);
    m_reflowFrom[m.key] = y;
  }
}

void NotificationCenterPanel::playReflow() {
  for (const Moving& m : m_moving) {
    auto it = m_reflowFrom.find(m.key);
    const bool entering = it == m_reflowFrom.end();
    if (entering) {
      it = m_reflowFrom.find(m.from);
    }
    if (it == m_reflowFrom.end()) {
      continue;
    }
    float x = 0.0F;
    float y = 0.0F;
    Node::absolutePosition(m.node, x, y);
    const float dy = it->second - y;
    Node* node = m.node;
    if (std::abs(dy) < 0.5F || m_animations == nullptr) {
      continue;
    }
    // Cards are translucent: an entering card shows only in the last third of its travel, once it is mostly clear of
    // the card it slides out from under.
    node->setTranslation(0.0F, dy);
    if (entering) {
      node->setOpacity(0.0F);
    }
    m_animations->animateSpatial(
        dy, 0.0F, Style::spring(Tokens::springMove),
        [node, dy, entering](float v) {
          node->setTranslation(0.0F, v);
          if (entering) {
            node->setOpacity(std::clamp(1.0F - 3.0F * v / dy, 0.0F, 1.0F));
          }
        },
        [node] { node->setOpacity(1.0F); }, node
    );
  }
  m_reflowFrom.clear();
}

// Collapse mirrors expand: the group's back cards slide under its front card while the footer and everything below
// move up by the height they free; the rebuild then lands on the same layout, and playReflow absorbs any rest.
void NotificationCenterPanel::collapseGroup(const std::string& key) {
  const std::string footerKey = "f:" + key;
  const auto footer = std::find_if(m_moving.begin(), m_moving.end(), [&](const Moving& m) { return m.key == footerKey; });
  if (footer == m_moving.end() || m_animations == nullptr) {
    m_openGroups.erase(key);
    requestRebuild();
    return;
  }
  const std::string front = footer == m_moving.begin() ? std::string{} : std::prev(footer)->from;
  const auto first = std::find_if(m_moving.begin(), footer, [&](const Moving& m) { return m.key == front; });
  if (first == footer) {
    m_openGroups.erase(key);
    requestRebuild();
    return;
  }
  const auto bottomOf = [](const Node* node) {
    float x = 0.0F;
    float y = 0.0F;
    Node::absolutePosition(node, x, y);
    return y + node->height();
  };
  const float frontBottom = bottomOf(first->node);
  const auto backs = static_cast<std::size_t>(std::distance(first, footer)) - 1;
  const std::size_t peeks = std::min(backs, static_cast<std::size_t>(Tokens::componentNotificationStackVisible) - 1);
  const float shift = bottomOf(std::prev(footer)->node) - frontBottom
                      - Tokens::componentNotificationStackOffset * contentScale() * static_cast<float>(peeks);
  for (auto it = std::next(first); it != footer; ++it) {
    Node* node = it->node;
    const float dy = frontBottom - bottomOf(node);
    m_animations->animateSpatial(
        0.0F, dy, Style::spring(Tokens::springMove),
        [node, dy](float v) {
          node->setTranslation(0.0F, v);
          node->setOpacity(std::clamp(1.0F - 3.0F * v / dy, 0.0F, 1.0F));
        },
        {}, node
    );
  }
  for (auto it = footer; it != m_moving.end(); ++it) {
    Node* node = it->node;
    const bool last = std::next(it) == m_moving.end();
    m_animations->animateSpatial(
        0.0F, -shift, Style::spring(Tokens::springMove), [node](float v) { node->setTranslation(0.0F, v); },
        last ? std::function<void()>([this, key] {
          captureReflow();
          m_openGroups.erase(key);
          requestRebuild();
        })
             : std::function<void()>{},
        node
    );
  }
}

void NotificationCenterPanel::rebuild() {
  if (m_list == nullptr) {
    return;
  }
  m_dirty = false;
  m_cards.clear();
  m_peeks.clear();
  m_moving.clear();
  m_icons.clear();
  while (!m_list->children().empty()) {
    m_list->removeChild(m_list->children().back().get());
  }
  const float scale = contentScale();
  std::vector<nc::Group> groups =
      m_notifications != nullptr ? nc::buildGroups(m_notifications->history(), m_openGroups) : std::vector<nc::Group>{};
  m_key = contentKey();
  if (groups.empty()) {
    auto empty = ui::row(
        {.align = FlexAlign::Center,
         .justify = FlexJustify::Center,
         .padding = Tokens::spaceP12 * scale,
         .minHeight = Tokens::sizeControlXl * scale,
         .fillWidth = true},
        ui::label({
            .text = i18n::tr("control-center.detail.notifications.empty"),
            .fontSize = Tokens::typeBody.size * scale,
            .color = colorSpecFromRole(ColorRole::Label2),
        })
    );
    m_list->addChild(std::move(empty));
    return;
  }
  for (const nc::Group& group : groups) {
    addGroup(*m_list, group);
  }
}

void NotificationCenterPanel::addGroup(Flex& list, const nc::Group& group) {
  const float scale = contentScale();
  const nc::GroupView view = nc::viewOf(group, static_cast<std::size_t>(Tokens::componentNotificationStackVisible));
  auto column = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * scale, .fillWidth = true});
  auto stack = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * scale, .fillWidth = true});
  // Back cards come first: they draw under the front card.
  for (std::size_t k = view.peeks; k >= 1; --k) {
    auto peek = ui::row({.participatesInLayout = false});
    peek->setClipChildren(true);
    auto* back = static_cast<Flex*>(peek->addChild(ui::row({.participatesInLayout = false})));
    styleCard(*back, scale);
    m_peeks.push_back({.node = peek.get(), .card = back, .depth = static_cast<int>(k)});
    m_moving.push_back({.key = "p:" + group.key + ":" + std::to_string(k), .from = "c:" + std::to_string(group.items.front()->id), .node = peek.get()});
    stack->addChild(std::move(peek));
  }
  const std::size_t firstPeek = m_peeks.size() - view.peeks;
  for (std::size_t i = 0; i < view.cards; ++i) {
    auto card = makeCard(*group.items[i]);
    m_moving.push_back({.key = "c:" + std::to_string(group.items[i]->id), .from = "c:" + std::to_string(group.items.front()->id), .node = card.get()});
    // Each card draws under the one above it, so an expanding stack slides out from under its front card.
    card->setZIndex(-static_cast<std::int32_t>(i));
    if (i == 0) {
      for (std::size_t p = firstPeek; p < m_peeks.size(); ++p) {
        m_peeks[p].front = card.get();
      }
    }
    stack->addChild(std::move(card));
  }
  if (view.peeks > 0) {
    stack->setPadding(0.0F, 0.0F, Tokens::componentNotificationStackOffset * static_cast<float>(view.peeks) * scale, 0.0F);
  }
  column->addChild(std::move(stack));

  if (view.footer) {
    const std::string key = group.key;
    const bool open = group.open;
    std::vector<std::uint32_t> ids;
    for (const Notification* n : group.items) {
      ids.push_back(n->id);
    }
    auto footer = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Tokens::spaceP8 * scale, .fillWidth = true});
    const std::string toggle = open ? i18n::tr("control-center.notifications.collapse-group")
                                    : i18n::tr("control-center.notifications.group-more", "count", static_cast<int>(group.items.size() - 1));
    footer->addChild(plainPill(toggle, scale, [this, key, open] {
      if (open) {
        collapseGroup(key);
        return;
      }
      captureReflow();
      m_openGroups.insert(key);
      requestRebuild();
    }, open ? "chevron-up" : "chevron-down"));
    footer->addChild(plainPill(i18n::tr("control-center.detail.notifications.clear"), scale, [this, ids] {
      for (const std::uint32_t id : ids) {
        m_notifications->removeHistoryEntry(id);
      }
      requestRebuild();
    }));
    m_moving.push_back({.key = "f:" + key, .node = footer.get()});
    column->addChild(std::move(footer));
  }
  list.addChild(std::move(column));
}

std::unique_ptr<Flex> NotificationCenterPanel::makeCard(const Notification& n) {
  const float scale = contentScale();
  const float tile = Tokens::sizeControlM * scale;
  auto card = ui::row({.align = FlexAlign::Start, .gap = Tokens::spaceP12 * scale, .padding = Tokens::spaceP12 * scale,
                       .fillWidth = true});
  styleCard(*card, scale);

  IconSlot slot;
  auto frame = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::Center, .fill = colorSpecFromRole(ColorRole::Fill1),
                        .radius = Style::scaledRadius(Tokens::radiusR8, scale), .minWidth = tile, .minHeight = tile});
  frame->setMaxWidth(tile);
  auto image = std::make_unique<Image>();
  image->setFit(ImageFit::Cover);
  image->setSize(tile, tile);
  image->setRadius(Style::scaledRadius(Tokens::radiusR8, scale));
  image->setParticipatesInLayout(false);
  image->setVisible(false);
  slot.image = image.get();
  frame->addChild(ui::glyph({.out = &slot.fallback, .glyph = "bell", .glyphSize = Tokens::sizeSymbol * scale, .color = colorSpecFromRole(ColorRole::Label1)}));
  frame->addChild(std::move(image));
  if (n.imageData.has_value()) {
    slot.raw = std::make_shared<const NotificationImageData>(*n.imageData);
  }
  if (n.icon.has_value()) {
    slot.candidates.push_back(*n.icon);
  }
  if (n.desktopEntry.has_value()) {
    slot.candidates.push_back(*n.desktopEntry);
  }
  if (slot.raw != nullptr || !slot.candidates.empty()) {
    m_icons.push_back(std::move(slot));
  }
  card->addChild(std::move(frame));

  Card entry;
  const float small = Tokens::typeFootnote.size * scale;
  auto top = ui::row(
      {.align = FlexAlign::Center, .gap = Tokens::spaceP8 * scale, .fillWidth = true},
      ui::label({.text = notificationDisplayAppName(n), .fontSize = small, .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1,
                 .flexGrow = 1.0F}),
      ui::label({.out = &entry.age, .text = nc::ageText(n), .fontSize = small, .color = colorSpecFromRole(ColorRole::Label2)})
  );
  auto text = ui::column({.align = FlexAlign::Stretch, .fillWidth = true}, std::move(top));
  text->setFlexGrow(1.0F);
  text->addChild(ui::label({
      .text = n.summary.empty() ? notificationDisplayAppName(n) : n.summary,
      .fontSize = Tokens::typeHeadline.size * scale,
      .fontWeight = static_cast<FontWeight>(Tokens::typeHeadline.weight),
      .color = colorSpecFromRole(ColorRole::Label1),
      .maxLines = 1,
      .configure = [scale](Label& l) { l.setLineHeight(Tokens::typeHeadline.line * scale); },
  }));
  if (!n.body.empty()) {
    text->addChild(ui::label({
        .text = n.body,
        .fontSize = small,
        .color = colorSpecFromRole(ColorRole::Label2),
        .maxLines = 2,
        .configure = [scale](Label& l) { l.setLineHeight(Tokens::typeFootnote.line * scale); },
    }));
  }
  // The app's first named action (Reply, Open) as a pill under the text; "default" is the card click.
  Node* actionPtr = nullptr;
  std::string actionKey;
  if (m_notifications != nullptr && m_notifications->canInvokeActions(n.id)) {
    for (std::size_t a = 0; a + 1 < n.actions.size(); a += 2) {
      if (n.actions[a] != "default" && !n.actions[a + 1].empty()) {
        actionKey = n.actions[a];
        auto button = pill(n.actions[a + 1], scale, {});
        actionPtr = button.get();
        text->addChild(ui::row({.paddingV = Tokens::spaceP4 * scale}, std::move(button)));
        break;
      }
    }
  }
  card->addChild(std::move(text));

  // The clear mark replaces the age while the pointer is on the card (proto .o-x); one area takes hover and clicks.
  const float mark = Tokens::sizeControlS * scale;
  auto clear = ui::row({.out = &entry.clear, .align = FlexAlign::Center, .justify = FlexJustify::Center,
                        .fill = colorSpecFromRole(ColorRole::Fill1), .radius = mark * 0.5F, .width = mark, .height = mark,
                        .visible = false, .participatesInLayout = false},
                       ui::glyph({.glyph = "x", .glyphSize = Tokens::sizeSymbol * scale, .color = colorSpecFromRole(ColorRole::Label1)}));
  card->addChild(std::move(clear));
  auto area = std::make_unique<InputArea>();
  area->setParticipatesInLayout(false);
  area->setCursorShape(CursorShape::Pointer);
  entry.area = area.get();
  Flex* clearPtr = entry.clear;
  Label* agePtr = entry.age;
  const auto show = [clearPtr, agePtr](bool hover) {
    clearPtr->setVisible(hover);
    agePtr->setOpacity(hover ? 0.0F : 1.0F);
    PanelManager::instance().requestRedraw();
  };
  area->setOnEnter([show](const InputArea::PointerData&) { show(true); });
  area->setOnLeave([show] { show(false); });
  area->setOnClick([this, areaPtr = entry.area, clearPtr, actionPtr, actionKey, id = n.id](const InputArea::PointerData& p) {
    const auto on = [&p, areaPtr](const Node* node) {
      if (node == nullptr || !node->visible()) {
        return false;
      }
      float ax = 0.0F;
      float ay = 0.0F;
      float nx = 0.0F;
      float ny = 0.0F;
      Node::absolutePosition(areaPtr, ax, ay);
      Node::absolutePosition(node, nx, ny);
      const float x = ax + p.localX - nx;
      const float y = ay + p.localY - ny;
      return x >= 0.0F && x <= node->width() && y >= 0.0F && y <= node->height();
    };
    if (on(clearPtr)) {
      m_notifications->removeHistoryEntry(id);
      requestRebuild();
    } else {
      activate(id, on(actionPtr) ? actionKey : std::string("default"));
    }
  });
  card->addChild(std::move(area));
  entry.node = card.get();
  m_cards.push_back(entry);
  return card;
}

void NotificationCenterPanel::activate(std::uint32_t id, const std::string& action) {
  if (m_notifications == nullptr || !m_notifications->canInvokeActions(id)) {
    return;
  }
  const std::string token = m_platform != nullptr ? m_platform->requestActivationToken(m_platform->lastPointerSurface()) : std::string{};
  (void)m_notifications->invokeAction(id, action, token, true);
  PanelManager::instance().closePanel();
}

void NotificationCenterPanel::loadIcons(Renderer& renderer) {
  const int px = static_cast<int>(std::ceil(Tokens::sizeControlM * contentScale()));
  for (IconSlot& slot : m_icons) {
    if (slot.done) {
      continue;
    }
    slot.done = true;
    bool loaded = false;
    if (slot.raw != nullptr && slot.raw->width > 0 && slot.raw->height > 0 && !slot.raw->data.empty() && slot.raw->bitsPerSample == 8
        && (slot.raw->channels == 3 || slot.raw->channels == 4)) {
      const PixmapFormat format = slot.raw->channels == 3 ? PixmapFormat::RGB : PixmapFormat::RGBA;
      loaded = slot.image->setSourceRaw(renderer, slot.raw->data.data(), slot.raw->data.size(), slot.raw->width, slot.raw->height,
                                        slot.raw->rowStride, format, true);
    }
    for (std::size_t i = 0; !loaded && i < slot.candidates.size(); ++i) {
      const std::string& candidate = slot.candidates[i];
      const std::string path = !candidate.empty() && candidate.front() == '/' ? candidate : m_iconResolver.resolve(candidate, px);
      loaded = !path.empty() && slot.image->setSourceFile(renderer, path, px, true);
    }
    slot.image->setVisible(loaded);
    slot.fallback->setVisible(!loaded);
  }
}

void NotificationCenterPanel::doUpdate(Renderer& /*renderer*/) {
  if (m_list != nullptr && (m_dirty || contentKey() != m_key)) {
    rebuild();
    PanelManager::instance().requestLayout();
  }
}

float NotificationCenterPanel::measureContentHeight(Renderer& /*renderer*/, float /*contentWidth*/) {
  if (m_column == nullptr || m_list == nullptr) {
    return 0.0F;
  }
  // The scroll view lays its content out at its natural height.
  const float natural = (Tokens::sizeControlXl + Tokens::spaceP8) * contentScale() + m_list->height();
  return m_maxHeight > 0.0F ? std::min(natural, m_maxHeight) : natural;
}

void NotificationCenterPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_column == nullptr) {
    return;
  }
  loadIcons(renderer);
  const float scale = contentScale();
  // The first layout of an open gets the whole slot, later ones the fitted height (never more): the largest is the slot.
  // The column stops center_reserve - center_top above the slot's bottom.
  m_slotHeight = std::max(m_slotHeight, height);
  m_maxHeight = std::max(0.0F, m_slotHeight - (Tokens::componentNotificationCenterReserve - Tokens::componentNotificationCenterTop) * scale);
  m_column->setSize(width, height);
  m_column->layout(renderer);

  for (const Card& card : m_cards) {
    card.area->setPosition(0.0F, 0.0F);
    card.area->setSize(card.node->width(), card.node->height());
    const float inset = Tokens::spaceP8 * scale;
    card.clear->setPosition(card.node->width() - inset - card.clear->width(), inset);
  }
  for (const Peek& peek : m_peeks) {
    // A back card `depth` steps deep: scaled about its bottom centre, its bottom edge `depth` steps under the front card's.
    // Only the strip under the card in front shows, so the translucent fills never stack.
    const float s = 1.0F - (1.0F - Tokens::componentNotificationStackScale) * static_cast<float>(peek.depth);
    const float step = Tokens::componentNotificationStackOffset * scale;
    const float w = peek.front->width() * s;
    const float h = peek.front->height() * s;
    const float bottom = peek.front->y() + peek.front->height() + step * static_cast<float>(peek.depth);
    peek.node->setPosition(peek.front->x() + (peek.front->width() - w) * 0.5F, bottom - step);
    peek.node->setSize(w, step);
    peek.card->setPosition(0.0F, step - h);
    peek.card->setSize(w, h);
  }
  if (!m_dirty && !m_reflowFrom.empty()) {
    playReflow();
  }
}
