#include "shell/notification_center/notification_center_panel.h"

#include "compositors/compositor_platform.h"
#include "core/input/cursor_shape.h"
#include "i18n/i18n.h"
#include "notification/notification_display_name.h"
#include "notification/notification_manager.h"
#include "render/core/texture_manager.h"
#include "render/scene/input_area.h"
#include "shell/panel/glass_piece.h"
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

} // namespace

float NotificationCenterPanel::preferredWidth() const { return scaled(Tokens::componentNotificationCenterWidth); }

void NotificationCenterPanel::onOpen(std::string_view /*context*/) {
  if (m_notifications == nullptr) {
    return;
  }
  m_notifications->markNotificationHistorySeen();
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
  head->setPadding(0.0F, Tokens::spaceP8 * scale, 0.0F, Tokens::spaceP16 * scale);
  styleGlassPiece(*head, Tokens::sizeControlXl * 0.5F * scale, scale, panelCardOpacity());
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
    key += std::format("{}:{};", entry.notification.id, nc::ageText(entry.notification));
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

void NotificationCenterPanel::rebuild() {
  if (m_list == nullptr) {
    return;
  }
  m_dirty = false;
  m_cards.clear();
  m_peeks.clear();
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
    styleGlassPiece(*empty, Tokens::radiusR24 * scale, scale, panelCardOpacity());
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
  // Back cards come first: they draw (and their glass stacks) under the front card.
  std::vector<std::unique_ptr<Flex>> peeks;
  for (std::size_t k = view.peeks; k >= 1; --k) {
    auto peek = ui::row({.participatesInLayout = false});
    styleGlassPiece(*peek, Tokens::radiusR24 * scale, scale, panelCardOpacity());
    m_peeks.push_back({.node = peek.get(), .depth = static_cast<int>(k)});
    stack->addChild(std::move(peek));
  }
  const std::size_t firstPeek = m_peeks.size() - view.peeks;
  for (std::size_t i = 0; i < view.cards; ++i) {
    auto card = makeCard(*group.items[i]);
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
    footer->addChild(pill(toggle, scale, [this, key, open] {
      if (open) {
        m_openGroups.erase(key);
      } else {
        m_openGroups.insert(key);
      }
      requestRebuild();
    }));
    footer->addChild(pill(i18n::tr("control-center.detail.notifications.clear"), scale, [this, ids] {
      for (const std::uint32_t id : ids) {
        m_notifications->removeHistoryEntry(id);
      }
      requestRebuild();
    }));
    column->addChild(std::move(footer));
  }
  list.addChild(std::move(column));
}

std::unique_ptr<Flex> NotificationCenterPanel::makeCard(const Notification& n) {
  const float scale = contentScale();
  const float tile = Tokens::sizeControlM * scale;
  auto card = ui::row({.align = FlexAlign::Start, .gap = Tokens::spaceP12 * scale, .padding = Tokens::spaceP12 * scale, .fillWidth = true});
  styleGlassPiece(*card, Tokens::radiusR24 * scale, scale, panelCardOpacity());

  IconSlot slot;
  auto frame = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::Center, .fill = colorSpecFromRole(ColorRole::Fill1),
                        .radius = Tokens::radiusR8 * scale, .minWidth = tile, .minHeight = tile});
  frame->setMaxWidth(tile);
  auto image = std::make_unique<Image>();
  image->setFit(ImageFit::Cover);
  image->setSize(tile, tile);
  image->setRadius(Tokens::radiusR8 * scale);
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
  area->setOnClick([this, clearPtr, id = n.id](const InputArea::PointerData& p) {
    const bool onClear = p.localX >= clearPtr->x() && p.localX <= clearPtr->x() + clearPtr->width() && p.localY >= clearPtr->y()
        && p.localY <= clearPtr->y() + clearPtr->height();
    if (onClear) {
      m_notifications->removeHistoryEntry(id);
      requestRebuild();
    } else {
      activate(id);
    }
  });
  card->addChild(std::move(area));
  entry.node = card.get();
  m_cards.push_back(entry);
  return card;
}

void NotificationCenterPanel::activate(std::uint32_t id) {
  if (m_notifications == nullptr || !m_notifications->canInvokeActions(id)) {
    return;
  }
  const std::string token = m_platform != nullptr ? m_platform->requestActivationToken(m_platform->lastPointerSurface()) : std::string{};
  (void)m_notifications->invokeAction(id, "default", token, true);
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
    // Scaled about its centre, the box keeps its bottom edge `depth` steps under the front card's bottom (origin 50% 100%).
    const float s = 1.0F - (1.0F - Tokens::componentNotificationStackScale) * static_cast<float>(peek.depth);
    const float w = peek.front->width();
    const float h = peek.front->height();
    peek.node->setSize(w, h);
    peek.node->setScale(s);
    peek.node->setPosition(peek.front->x(), peek.front->y() + Tokens::componentNotificationStackOffset * scale * static_cast<float>(peek.depth)
                                                + h * (1.0F - s) * 0.5F);
  }
}
