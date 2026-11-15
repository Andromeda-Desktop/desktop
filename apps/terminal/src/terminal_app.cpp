#include "terminal_app.h"

#include "terminal_window.h"

namespace term {

  TerminalApp::TerminalApp() : DeskApp({.id = "dev.andromeda.Terminal", .name = "Terminal"}) {}

  TerminalApp::~TerminalApp() = default;

  AppWindow& TerminalApp::createWindow() {
    AppWindow& window = addWindow({.appId = info().id, .title = info().name, .width = 820, .height = 520});
    m_windows[&window] = std::make_unique<TerminalWindow>(loop(), window, [this, &window] { closeWindow(window); });
    return window;
  }

  void TerminalApp::windowClosed(AppWindow& window) { m_windows.erase(&window); }

} // namespace term
