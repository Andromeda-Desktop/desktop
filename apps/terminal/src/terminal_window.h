#pragma once

#include "session_list.h"
#include "terminal_view.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

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
    // origins: one session each, in order; the last one is shown. close: closes this window later, outside the
    // current callback (DeskApp::closeWindow).
    TerminalWindow(AppLoop& loop, AppWindow& window, Notifier& notifier, const std::vector<WindowOrigin>& origins,
                   std::function<void()> close);
    ~TerminalWindow();
    // The surface and scene were destroyed (compositor reconnect): the next requestFrame hooks the new surface.
    void surfaceLost();

    // Opens a session and shows it.
    void openSession(const WindowOrigin& origin);

  private:
    struct Session {
      std::uint64_t id;
      std::unique_ptr<TerminalView> view;
    };

    void requestFrame();
    void openUri(const std::string& uri);
    void apply(const SessionListEvent& event);
    void show(std::uint64_t id);
    // Session keys: Ctrl+Shift+T new, Ctrl+Tab / Ctrl+Shift+Tab recent, Ctrl+1..9 by position.
    bool sessionKey(const KeyboardEvent& event);
    TerminalView* shownView();

    AppLoop& m_loop;
    AppWindow& m_window;
    Notifier& m_notifier;
    std::function<void()> m_close;
    SessionList m_sessions;
    std::vector<Session> m_views;
    // Closed sessions whose nodes are still in the scene: freed after the next build replaced them.
    std::vector<Session> m_retired;
    std::uint64_t m_nextId = 1;
    std::uint64_t m_builtId = 0;
    bool m_active = false;
    Surface* m_tickSurface = nullptr;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace term
