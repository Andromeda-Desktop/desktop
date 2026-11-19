#pragma once

#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "shell/control_center/sheet/sheet_page.h"
#include "calendar/calendar_view.h"

#include <chrono>
#include <cstdint>
#include <limits>

class Button;
class CalendarService;
class CompositorPlatform;
class ConfigService;
class Glyph;
class InputArea;
class Label;
class ScrollView;
class WeatherService;

class CalendarPage : public SheetPage {
public:
  // Sheet: the Control Center clock sheet. Pieces: concept C (proto overlays.js renderCal), the month card and the
  // events card as glass pieces, no date header or weather tile.
  enum class Layout : std::uint8_t { Sheet, Pieces };
  explicit CalendarPage(
      ConfigService* config = nullptr, CalendarService* calendar = nullptr, WeatherService* weather = nullptr,
      CompositorPlatform* platform = nullptr, Layout layout = Layout::Sheet
  );

  // Height of the one-column clock sheet at this width (the panel fits the sheet to it).
  [[nodiscard]] float measureHeight(Renderer& renderer, float width);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void setActive(bool active) override;
  bool handleKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;
  void onClose() override;

private:
  void focusToday();
  void moveSelection(int days);
  void addEventsEmptyState(float scale);
  void changeMonthBy(int delta);
  void cancelMonthSlide();
  void beginSlideIn();
  void applyMonthSlide(float progress);
  [[nodiscard]] std::unique_ptr<Flex> makeGrid(float scale);
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void rebuild();
  void rebuildEventList(float scale);
  void toggleEventsCard();
  void updateWeather();
  void updateDateHeader();
  [[nodiscard]] float gridHeight(float scale) const;
  [[nodiscard]] float gridGap() const;
  [[nodiscard]] float dayButtonSize() const;

  Layout m_layout = Layout::Sheet;
  ConfigService* m_config = nullptr;
  CalendarService* m_calendar = nullptr;
  WeatherService* m_weather = nullptr;
  CompositorPlatform* m_platform = nullptr;
  Flex* m_eventsHeader = nullptr;
  Label* m_weekdayLabel = nullptr;
  Button* m_todayButton = nullptr;
  std::chrono::system_clock::time_point m_weatherFetchedAt{};
  Flex* m_weatherTile = nullptr;
  Glyph* m_weatherGlyph = nullptr;
  Label* m_weatherTitle = nullptr;
  Label* m_weatherDetail = nullptr;
  InputArea* m_weatherArea = nullptr;
  bool m_changeCallbackRegistered = false;
  bool m_eventsDirty = false;
  Flex* m_rootLayout = nullptr;
  Button* m_toggleEventsCardButton = nullptr;
  InputArea* m_calendarArea = nullptr;
  Flex* m_card = nullptr;
  Flex* m_header = nullptr;
  Flex* m_previousSlot = nullptr;
  Flex* m_nextSlot = nullptr;
  Flex* m_monthWrap = nullptr;
  Label* m_todayLabel = nullptr;
  Label* m_monthLabel = nullptr;
  Button* m_previousButton = nullptr;
  Button* m_nextButton = nullptr;
  Flex* m_gridViewport = nullptr;
  Flex* m_grid = nullptr;
  Flex* m_outgoingGrid = nullptr; // the month sliding out while m_grid slides in
  Flex* m_eventsCard = nullptr;
  Label* m_eventsTitle = nullptr;
  ScrollView* m_eventsScroll = nullptr;
  calendar_view::EventListState m_eventListState;
  int m_selectedYear = std::numeric_limits<int>::min();
  int m_selectedMonth = -1;
  int m_selectedDay = -1;
  int m_monthOffset = 0;
  float m_lastInnerWidth = -1.0F;
  float m_lastInnerHeight = -1.0F;
  int m_lastDisplayYear = std::numeric_limits<int>::min();
  int m_lastDisplayMonth = -1;
  int m_lastCurrentYear = std::numeric_limits<int>::min();
  int m_lastCurrentMonth = -1;
  int m_lastToday = -1;
  int m_monthSlideDirection = 0;
  // Pending: the new month's grid waits for its first layout. m_outgoingGrid is set exactly while not Idle.
  enum class MonthSlide : std::uint8_t { Idle, Pending, Running };
  MonthSlide m_monthSlide = MonthSlide::Idle;
  AnimationManager::Id m_monthSlideAnimId = 0;
  bool m_showEventsCard = true;
  bool m_showWeekNumbers = false;
  Timer m_eventFadeTimer;
};
