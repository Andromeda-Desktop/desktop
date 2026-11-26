#include "shell/bar/widgets/notification_widget.h"

#include "notification/notification_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/count_badge.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <memory>

namespace {
  constexpr float kDotBaseSize = 6.0F;
} // namespace

NotificationWidget::NotificationWidget(NotificationManager* manager, wl_output* /*output*/, Options options)
    : m_manager(manager), m_hideWhenNoUnread(options.hideWhenNoUnread), m_showCount(options.showCount) {}

void NotificationWidget::create() {
  auto area = ui::inputArea({});

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "bell",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = widgetIconColorOr(colorSpecFromRole(ColorRole::Label1)),
      })
  );

  if (m_showCount) {
    auto badge = ui::box({
        .out = &m_countBadge,
        .fill = colorSpecFromRole(ColorRole::Accent),
        .visible = false,
    });
    badge->addChild(ui::label({
        .out = &m_countLabel,
        .fontSize = Style::fontSizeCaption * m_contentScale,
        .fontWeight = FontWeight::Bold,
        .color = colorSpecFromRole(ColorRole::OnAccent),
    }));
    area->addChild(std::move(badge));
  } else {
    const float dotSize = kDotBaseSize * m_contentScale;
    m_dot = area->addChild(
        ui::box({
            .fill = colorSpecFromRole(ColorRole::Accent),
            .radius = dotSize * 0.5F,
            .width = dotSize,
            .height = dotSize,
            .visible = false,
        })
    );
  }

  setRoot(std::move(area));
  refreshIndicatorState();
}

// hide_when_no_unread would pull the widget out from under the pointer the moment the panel it
// just opened marks everything read. The latch clears itself once that panel closes.
void NotificationWidget::onGestureDispatch(noctalia::bar::Gesture gesture, const noctalia::bar::WidgetAction& action) {
  (void)gesture;
  (void)action;
  m_openedPanelByClick = true;
}

void NotificationWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* rootNode = root();
  if (m_glyph == nullptr || rootNode == nullptr) {
    return;
  }

  refreshIndicatorState();
  if (!rootNode->visible()) {
    rootNode->setSize(0.0F, 0.0F);
    return;
  }

  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_glyph->setGlyph(m_glyphName);
  m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::Label1)));
  m_glyph->measure(renderer);
  m_glyph->setPosition(0.0F, 0.0F);
  rootNode->setSize(m_glyph->width(), m_glyph->height());

  if (m_dot != nullptr) {
    const float dotSize = kDotBaseSize * m_contentScale;
    // Inside the symbol box, not the §9.4 overhang: bar slots clip to the widget bounds (deviations.md).
    m_dot->setPosition(m_glyph->width() - dotSize, 0.0F);
  }
  layoutCountBadge(renderer);
}

// Inside the symbol box, top right, like the dot: bar slots clip to the widget bounds.
void NotificationWidget::layoutCountBadge(Renderer& renderer) {
  if (m_countBadge == nullptr || m_countLabel == nullptr || m_glyph == nullptr) {
    return;
  }
  const float height = Tokens::componentBadgeHeight * m_contentScale;
  const float width = count_badge::layout(renderer, *m_countBadge, *m_countLabel, m_unreadCount, height, m_contentScale);
  m_countBadge->setPosition(std::max(0.0F, m_glyph->width() - width), 0.0F);
}

void NotificationWidget::doUpdate(Renderer& renderer) {
  const std::size_t unread = m_unreadCount;
  refreshIndicatorState();
  if (m_unreadCount != unread) {
    layoutCountBadge(renderer);
  }
}

void NotificationWidget::refreshIndicatorState() {
  const bool hasNotifications = (m_manager != nullptr) && m_manager->hasUnreadNotificationHistory();
  const std::size_t unread = m_manager != nullptr ? m_manager->unreadNotificationCount() : 0;
  const bool dndEnabled = (m_manager != nullptr) && m_manager->doNotDisturb();

  if (Node* rootNode = root(); rootNode != nullptr) {
    if (m_openedPanelByClick) {
      m_openedPanelByClick = PanelManager::instance().isOpenPanel("control-center");
    }
    const bool showWidget = m_openedPanelByClick || !m_hideWhenNoUnread || hasNotifications;
    rootNode->setVisible(showWidget);
    rootNode->setParticipatesInLayout(showWidget);
    if (!showWidget) {
      rootNode->setSize(0.0F, 0.0F);
    }
  }

  // The bell shows the glyph of the Focus that is on (docs/focus.md §6 F2).
  std::string glyph = "bell";
  if (const auto& active = m_manager->focus().active(); dndEnabled && active.has_value()) {
    if (const focus::Focus* on = m_manager->focus().find(active->focusId); on != nullptr) {
      glyph = on->glyph;
    }
  }
  if (hasNotifications == m_hasNotifications && dndEnabled == m_dndEnabled && glyph == m_glyphName
      && unread == m_unreadCount) {
    return;
  }
  m_unreadCount = unread;
  m_hasNotifications = hasNotifications;
  m_dndEnabled = dndEnabled;
  m_glyphName = glyph;
  if (m_glyph != nullptr) {
    m_glyph->setGlyph(m_glyphName);
    m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::Label1)));
  }
  if (m_dot != nullptr) {
    m_dot->setVisible(m_hasNotifications && !m_dndEnabled);
  }
  if (m_countBadge != nullptr) {
    m_countBadge->setVisible(m_unreadCount > 0 && !m_dndEnabled);
  }
}
