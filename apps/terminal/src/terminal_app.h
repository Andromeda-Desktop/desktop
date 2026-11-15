#pragma once

#include "app/desk_app.h"
#include "terminal_window.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace term {

  class Notifier;

  // andromeda-terminal: one process; each window runs one shell.
  class TerminalApp final : public DeskApp {
  public:
    TerminalApp();
    ~TerminalApp() override;

  protected:
    AppWindow& createWindow() override;
    void windowClosed(AppWindow& window) override;
    void windowSurfaceLost(AppWindow& window) override;
    // Each directory among the files opens a window there (andromeda-terminal <dir>, Files "Open Terminal Here").
    void open(const std::vector<std::string>& uris, const std::string& activationToken) override;
    // With no window yet, sessions that lost their terminal process come back first, one window each.
    void activate(const std::string& activationToken) override;

  private:
    std::map<AppWindow*, std::unique_ptr<TerminalWindow>> m_windows;
    std::unique_ptr<Notifier> m_notifier;
    WindowOrigin m_nextOrigin; // what the window createWindow() makes next runs
  };

} // namespace term
