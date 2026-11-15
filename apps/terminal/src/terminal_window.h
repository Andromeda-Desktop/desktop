#pragma once

#include "terminal_view.h"

#include <functional>

class AppLoop;
class AppWindow;
class Surface;

namespace term {

  class TerminalWindow {
  public:
    // close: closes this window later, outside the current callback (DeskApp::closeWindow).
    TerminalWindow(AppLoop& loop, AppWindow& window, std::function<void()> close);
    ~TerminalWindow();

  private:
    void requestFrame();

    AppLoop& m_loop;
    AppWindow& m_window;
    TerminalView m_view;
    Surface* m_tickSurface = nullptr;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace term
