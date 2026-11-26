#pragma once

#include "shell/bar/widget.h"

struct wl_output;
class Box;
class Glyph;
class Label;
class Node;
class NotificationManager;

class NotificationWidget : public Widget {
public:
  struct Options {
    bool hideWhenNoUnread = false;
    // The unread count in a badge (99+) instead of the dot.
    bool showCount = false;
  };

  NotificationWidget(NotificationManager* manager, wl_output* output, Options options);

  void create() override;

private:
  void onGestureDispatch(noctalia::bar::Gesture gesture, const noctalia::bar::WidgetAction& action) override;
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void refreshIndicatorState();
  void layoutCountBadge(Renderer& renderer);

  NotificationManager* m_manager = nullptr;
  Glyph* m_glyph = nullptr;
  Node* m_dot = nullptr;
  Box* m_countBadge = nullptr;
  Label* m_countLabel = nullptr;
  std::size_t m_unreadCount = 0;
  bool m_hideWhenNoUnread = false;
  bool m_showCount = false;
  bool m_hasNotifications = false;
  bool m_dndEnabled = false;
  std::string m_glyphName = "bell";
  bool m_openedPanelByClick = false;
};
