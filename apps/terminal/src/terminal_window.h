#pragma once

#include "terminal_view.h"

#include <cstdint>
#include <functional>
#include <string>
#include <variant>

class AppLoop;
class AppWindow;
class Surface;

namespace term {

  class Notifier;

  // cwd: the shell's start directory; empty starts in $HOME.
  struct NewShell {
    std::string cwd;
  };
  // A session in andromeda-ptyd that lost its terminal process.
  struct Reattach {
    std::uint64_t heldId;
  };
  using WindowOrigin = std::variant<NewShell, Reattach>;

  class TerminalWindow {
  public:
    // close: closes this window later, outside the current callback (DeskApp::closeWindow).
    TerminalWindow(AppLoop& loop, AppWindow& window, Notifier& notifier, const WindowOrigin& origin,
                   std::function<void()> close);
    ~TerminalWindow();
    // The surface and scene were destroyed (compositor reconnect): the next requestFrame hooks the new surface.
    void surfaceLost();

  private:
    void requestFrame();
    void openUri(const std::string& uri);

    AppLoop& m_loop;
    AppWindow& m_window;
    Notifier& m_notifier;
    TerminalView m_view;
    Surface* m_tickSurface = nullptr;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace term
