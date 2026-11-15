#pragma once

#include "app/desk_app.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace term {

  class Notifier;
  class TerminalWindow;

  // andromeda-terminal: one process; each window runs one shell.
  class TerminalApp final : public DeskApp {
  public:
    TerminalApp();
    ~TerminalApp() override;

  protected:
    AppWindow& createWindow() override;
    void windowClosed(AppWindow& window) override;
    // Each directory among the files opens a window there (andromeda-terminal <dir>, Files "Open Terminal Here").
    void open(const std::vector<std::string>& uris, const std::string& activationToken) override;

  private:
    std::map<AppWindow*, std::unique_ptr<TerminalWindow>> m_windows;
    std::unique_ptr<Notifier> m_notifier;
    std::string m_nextCwd; // start directory for the window createWindow() makes next
  };

} // namespace term
