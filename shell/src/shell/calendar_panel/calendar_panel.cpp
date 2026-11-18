#include "shell/calendar_panel/calendar_panel.h"

#include "ui/controls/flex.h"
#include "ui/style.h"

float CalendarPanel::preferredWidth() const { return scaled(Tokens::componentCalendarWidth); }

void CalendarPanel::create() {
  m_page.setContentScale(contentScale());
  m_page.setPanelCardOpacity(panelCardOpacity());
  setRoot(m_page.create());
  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }
}

void CalendarPanel::onOpen(std::string_view /*context*/) { m_page.setActive(true); }

void CalendarPanel::onClose() {
  m_page.setActive(false);
  m_page.onClose();
}
