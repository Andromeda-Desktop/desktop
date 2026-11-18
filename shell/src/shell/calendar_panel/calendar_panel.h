#pragma once

#include "shell/control_center/sheet/calendar_page.h"
#include "shell/panel/panel.h"

// Concept C calendar (proto overlays.js renderCal): the month card and today's events as two glass pieces in a 320
// column under the island; the clock and the island's date open it.
class CalendarPanel final : public Panel {
public:
  CalendarPanel(ConfigService* config, CalendarService* calendar, CompositorPlatform* platform)
      : m_page(config, calendar, nullptr, platform, CalendarPage::Layout::Pieces) {}

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override {
    return m_page.handleKey(sym, modifiers, pressed, preedit);
  }

  [[nodiscard]] float preferredWidth() const override;
  [[nodiscard]] float preferredHeight() const override { return scaled(4096.0F); }
  [[nodiscard]] bool fitsContentHeight() const noexcept override { return true; }
  [[nodiscard]] bool padsContent() const noexcept override { return false; }
  [[nodiscard]] bool usesGlassPieces() const override { return true; }
  [[nodiscard]] PiecePour piecePour() const override { return {.xShare = 0.0F, .yShare = 0.4F}; }
  [[nodiscard]] float measureContentHeight(Renderer& renderer, float contentWidth) override {
    return m_page.measureHeight(renderer, contentWidth);
  }
  [[nodiscard]] std::string panelScreenPosition() const override { return "top_center"; }

protected:
  void doLayout(Renderer& renderer, float width, float height) override { m_page.layout(renderer, width, height); }
  void doUpdate(Renderer& renderer) override { m_page.update(renderer); }

private:
  CalendarPage m_page;
};
