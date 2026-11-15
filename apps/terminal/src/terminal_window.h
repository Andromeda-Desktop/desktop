#pragma once

#include "terminal_view.h"

#include <functional>

class AppLoop;
class AppWindow;
class Surface;

namespace term {

  class Notifier;

  class TerminalWindow {
  public:
    // close: closes this window later, outside the current callback (DeskApp::closeWindow).
    // cwd: the shell's start directory; empty starts in $HOME.
    TerminalWindow(AppLoop& loop, AppWindow& window, Notifier& notifier, const std::string& cwd,
                   std::function<void()> close);
    ~TerminalWindow();

  private:
    void requestFrame();

    AppLoop& m_loop;
    AppWindow& m_window;
    Notifier& m_notifier;
    TerminalView m_view;
    Surface* m_tickSurface = nullptr;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace term
