#pragma once

#include "settings_store.h"
#include "core/timer_manager.h"
#include "session_list.h"
#include "ui/controls/status_row.h"
#include "terminal_view.h"
#include "agent_view.h"

#include <array>
#include <cstdint>
#include <optional>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

class AppLoop;
class AppWindow;
class Button;
class Label;
class Node;
class Input;
class Renderer;
class SelectionList;
class Surface;
class ThumbnailService;

namespace term {

  class Notifier;
  namespace agent {
    class Host;
  }

  // cwd: the shell's start directory; empty starts in $HOME.
  struct NewShell {
    std::string cwd;
  };
  // A session in andromeda-ptyd that lost its terminal process.
  struct Reattach {
    std::uint64_t heldId;
  };
  // An agent session (ACP) in cwd; empty starts in $HOME.
  struct NewAgent {
    std::string cwd;
  };
  using WindowOrigin = std::variant<NewShell, Reattach, NewAgent>;

  class TerminalWindow {
  public:
    // origins: one session each, in order; the last one is shown. close: closes this window later, outside the
    // current callback (DeskApp::closeWindow). newWindow: opens another window (Ctrl+Shift+N) with this
    // activation token.
    TerminalWindow(AppLoop& loop, AppWindow& window, Notifier& notifier, agent::Host& agents, ThumbnailService& thumbnails,
                   SettingsStore& settings,
                   const std::vector<WindowOrigin>& origins,
                   std::function<void()> close, std::function<void(const std::string& token)> newWindow);
    ~TerminalWindow();
    // The surface and scene were destroyed (compositor reconnect): the next requestFrame hooks the new surface.
    void surfaceLost();

    // Opens a session and shows it.
    void openSession(const WindowOrigin& origin);
    // An agent session of any window changed: repaints its view and row when it is in this window.
    void agentChanged(std::uint64_t key);

  private:
    struct Session {
      std::uint64_t id;
      std::unique_ptr<ContentView> view;
    };

    void requestFrame();
    void openUri(const std::string& uri);
    void apply(const SessionListEvent& event);
    void show(std::uint64_t id);
    // Session keys: Ctrl+Shift+T new, Ctrl+Shift+A new agent session, Ctrl+Shift+W close an agent session,
    // Ctrl+Shift+N new window, Ctrl+Tab / Ctrl+Shift+Tab recent, Ctrl+1..9 by position.
    bool sessionKey(const KeyboardEvent& event);
    void openAgentHere();
    void openNewMenu();
    ContentView* shownView();
    TerminalView* shownTerminal();

    AppLoop& m_loop;
    AppWindow& m_window;
    Notifier& m_notifier;
    agent::Host& m_agents;
    ThumbnailService& m_thumbnails;
    SettingsStore& m_settings;
    Signal<>::ScopedConnection m_settingsChanged;
    std::function<void()> m_close;
    std::function<void(const std::string& token)> m_newWindow;
    SessionList m_sessions;
    std::string m_filter; // the sidebar search text
    Input* m_search = nullptr;
    bool m_rail = false; // the sidebar is an icon rail
    Flex* m_agentBar = nullptr; // title band pills of the shown agent session
    void syncAgentBar();
    void fillAgentBar();
    [[nodiscard]] AgentView* agentView(std::uint64_t key);
    Button* m_newButton = nullptr; // the sidebar "+": its menu opens new sessions and windows
    SelectionList* m_rowList = nullptr;
    std::uint64_t m_tintTarget = 0; // the row the tint moves to
    std::uint64_t m_rowSelected = 0; // the row with selected labels: it changes at half the tint's travel
    std::vector<std::pair<std::uint64_t, StatusRow*>> m_rows; // session id, its sidebar row
    Timer m_tick;
    Timer m_agentTick;             // 1 Hz while the shown agent session runs a turn
    Label* m_bandStatus = nullptr; // the shown agent's status pill label
    [[nodiscard]] StatusRow::Data rowData(const SessionInfo& session) const;
    void refreshRows();
    [[nodiscard]] std::unique_ptr<StatusRow> makeRow(std::uint64_t id);
    [[nodiscard]] bool matches(const SessionInfo& session) const;
    void setFilter(const std::string& text);
    void applyFilter();
    void openFirstMatch();
    void focusGrid();
    // Find bar in the title band (Ctrl+Shift+F): it searches the shown session.
    void openFind();
    void closeFind();
    void setFindCount(std::size_t total, std::optional<std::size_t> current);
    bool m_findOpen = false;
    std::string m_findQuery;
    Node* m_findBar = nullptr;
    Input* m_findInput = nullptr;
    Label* m_findCount = nullptr;
    std::array<Button*, 2> m_findSteps{}; // prev, next
    std::vector<Session> m_views;
    // Closed sessions whose nodes are still in the scene: freed after the next build replaced them.
    std::vector<Session> m_retired;
    std::uint64_t m_nextId = 1;
    // Agent session keys are unique in the app: all windows share one host.
    static inline std::uint64_t s_nextAgentKey = 0;
    std::uint64_t m_builtId = 0;
    Node* m_contentSlot = nullptr; // holds the shown view's nodes
    float m_contentWidth = 0.0F;
    float m_contentHeight = 0.0F;
    void fillSlot(Renderer& renderer);
    bool m_active = false;
    Surface* m_tickSurface = nullptr;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace term
