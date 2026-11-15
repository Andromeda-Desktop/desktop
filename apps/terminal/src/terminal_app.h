#pragma once

#include "app/desk_app.h"

#include <map>
#include <memory>

namespace term {

  class TerminalWindow;

  // andromeda-terminal: one process; each window runs one shell.
  class TerminalApp final : public DeskApp {
  public:
    TerminalApp();
    ~TerminalApp() override;

  protected:
    AppWindow& createWindow() override;
    void windowClosed(AppWindow& window) override;

  private:
    std::map<AppWindow*, std::unique_ptr<TerminalWindow>> m_windows;
  };

} // namespace term
