#pragma once

#include "command_palette.h"
#include "settings_store.h"
#include "core/timer_manager.h"
#include "session_layout.h"
#include "session_list.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/status_row.h"
#include "terminal_view.h"
#include "agent_view.h"
#include "app/app_window.h"
#include "app/split_view.h"
#include "ui/split_layout.h"

#include <array>
#include <map>
#include <unordered_map>
#include <cstdint>
#include <optional>
#include <unordered_set>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

class AppLoop;
class AppWindow;
class Box;
class Button;
class Flex;
class Glyph;
class InputArea;
class Label;
class Spinner;
class Node;
class Input;
class Renderer;
class SelectionList;
class Surface;
class ThumbnailService;

struct wl_surface;

namespace term {

  [[nodiscard]] inline AppWindow::Material appMaterial(WindowMaterial m) {
    switch (m) {
    case WindowMaterial::Solid: return AppWindow::Material::Solid;
    case WindowMaterial::Glass: return AppWindow::Material::Glass;
    case WindowMaterial::GlassSidebar: return AppWindow::Material::GlassSidebar;
    case WindowMaterial::GlassContent: return AppWindow::Material::GlassContent;
    }
    return AppWindow::Material::Solid;
  }

  class Notifier;
  namespace agent {
    class Host;
  }

  // Unsaved: the change stays out of the saved layout until the next saved change (a shell killed at logout).
  enum class LayoutSave : std::uint8_t { Saved, Unsaved };

  // cwd: the shell's start directory; empty starts in $HOME.
  struct NewShell {
    std::string cwd;
    std::vector<std::string> argv; // a command to run instead of the user's shell; empty: the shell
  };
  // A session in andromeda-ptyd that lost its terminal process.
  struct Reattach {
    std::uint64_t heldId;
  };
  // An agent session (ACP) in cwd; empty starts in $HOME.
  struct NewAgent {
    agent::Kind kind;
    std::string cwd;
    std::string prompt; // sent as the first message; empty: none
  };
  // A live session leaving its window ("Move to new window"): its view moves as is; nothing restarts.
  struct MovedSession {
    std::uint64_t id;
    std::unique_ptr<ContentView> view;
    SessionInfo info;
    bool secret = false;
    bool remote = false;
  };
  struct Adopt {
    std::shared_ptr<MovedSession> session; // shared: WindowOrigin stays copyable; the adopting window takes the view
  };
  using WindowOrigin = std::variant<NewShell, Reattach, NewAgent, Adopt>;
  // The app's shared host for a kind; made on first use.
  using AgentHosts = std::function<agent::Host&(agent::Kind)>;

  // Asks where to save a file named like suggested (the portal dialog, parented to this window); nullopt = cancelled.
  using ChooseSaveFile = std::function<void(const std::string& suggested,
                                            std::function<void(std::optional<std::filesystem::path>)> picked)>;

  class TerminalWindow {
  public:
    // The window goes away but its held shells keep running (a closed window, not closed sessions).
    // Returns true when a held shell still runs.
    bool detachSessions();
    // origins: one session each, in order; the last one is shown. close: closes this window later, outside the
    // current callback (DeskApp::closeWindow). newWindow: opens another window (Ctrl+Shift+N) with this
    // activation token. joinWorkspace: puts each shell this window starts (held id, start folder) in its workspace.
    TerminalWindow(AppLoop& loop, AppWindow& window, Notifier& notifier, AgentHosts agents, ThumbnailService& thumbnails,
                   SettingsStore& settings,
                   const std::vector<WindowOrigin>& origins,
                   std::function<void()> close, std::function<void(LayoutSave)> sessionsChanged,
                   std::function<void(const std::string& token, std::vector<WindowOrigin> origins)> newWindow,
                   std::function<void(const std::string& token)> preferences, ChooseSaveFile chooseSaveFile,
                   std::function<void(std::uint64_t heldId, const std::string& dir)> joinWorkspace);
    ~TerminalWindow();
    // The surface and scene were destroyed (compositor reconnect): the next requestFrame hooks the new surface.
    void surfaceLost();

    // Opens a session and shows it; returns its id.
    std::uint64_t openSession(const WindowOrigin& origin);
    // The shell sessions to reopen after a reboot.
    [[nodiscard]] SavedWindow layout() const;
    // Opens the saved sessions with their names and splits, and shows the saved one.
    void restore(const SavedWindow& saved);
    [[nodiscard]] TerminalView::Host terminalHost(std::uint64_t id);
    void adoptSession(std::uint64_t id, MovedSession moved);
    void moveToNewWindow(std::uint64_t id);
    [[nodiscard]] bool canJoinShown(std::uint64_t id) const;
    void joinShown(std::uint64_t id);
    void ctrlClickRow(std::uint64_t id);
    // An agent session of any window changed: repaints its view and row when it is in this window.
    void agentChanged(std::uint64_t key);

    // Remote control (org.andromeda.Terminal1). Each acts only on a session of this window.
    [[nodiscard]] std::uint32_t windowId() const noexcept { return m_windowId; }
    [[nodiscard]] const SessionList& sessions() const noexcept { return m_sessions; }
    [[nodiscard]] TerminalView* terminalOf(std::uint64_t id); // null: no such session, or an agent session
    void focusSession(std::uint64_t id); // shows it and raises the window
    void closeSession(std::uint64_t id); // with no confirm sheet
    void renameSession(std::uint64_t id, std::string name); // empty: the automatic name
    // All windows of the app, in opening order (the command palette lists their sessions).
    void setWindows(std::function<std::vector<TerminalWindow*>()> windows) { m_windows = std::move(windows); }
    void setPaletteRecent(std::function<std::vector<std::string>()> load, std::function<void(std::vector<std::string>)> save) {
      m_loadRecent = std::move(load);
      m_saveRecent = std::move(save);
    }

  private:
    struct Session {
      std::uint64_t id;
      std::unique_ptr<ContentView> view;
    };

    void requestFrame();
    void openUri(const std::string& uri);
    void apply(const SessionListEvent& event, LayoutSave save = LayoutSave::Saved);
    [[nodiscard]] Settings viewSettings() const; // the setting with this window's zoom
    void applyViewSettings();
    void updateSceneState(); // scene busy, secret and remote follow the visible panes
    std::unordered_set<std::uint64_t> m_secretSessions; // sessions at a password prompt
    void toggleSecureEntry();
    // A caption shows a lock in place of its symbol while keys to session `id` are secure.
    void lockSymbol(std::uint64_t id, StatusRow::Data& data) const;
    bool m_secureEntry = false; // the user's secure keyboard entry toggle (Ctrl+Shift+S)
    bool m_keysSecure = false; // m_secureEntry or a visible password prompt: no IME, no compositor shortcuts, no capture
    std::unordered_set<std::uint64_t> m_remoteSessions; // sessions on another host (SSH, container)
    void show(std::uint64_t id);
    // Runs the window action bound to the key ([keybindings]); false for other keys and the grid's actions.
    bool sessionKey(const KeyboardEvent& event);
    bool runAction(Action action);
    // Command palette (Ctrl+Shift+P) docked to the focused pane. SplitWith lists only sessions that can join it.
    enum class PaletteTarget : std::uint8_t { Any, SplitWith };
    struct PaletteClosed {};
    struct PaletteOpen {
      std::shared_ptr<CommandPalette> ui; // shared: the close motion keeps it until its root leaves the scene
      Node* root = nullptr;
      std::unordered_map<std::string, std::function<void()>> runs; // by item id
      std::optional<std::string> preview; // the theme id this window shows while its row is current
    };
    std::variant<PaletteClosed, PaletteOpen> m_palette;
    // The app's saved palette picks (item ids, newest first); empty until setPaletteRecent.
    std::function<std::vector<std::string>()> m_loadRecent;
    std::function<void(std::vector<std::string>)> m_saveRecent;
    std::function<std::vector<TerminalWindow*>()> m_windows;
    std::function<void(std::uint64_t, const std::string&)> m_joinWorkspace;
    void openPalette(PaletteTarget target = PaletteTarget::Any);
    void closePalette(bool rootGone = false); // rootGone: the slot it was in was rebuilt
    [[nodiscard]] std::vector<palette::Item> paletteItems(PaletteTarget target,
                                                          std::unordered_map<std::string, std::function<void()>>& runs);
    void openAgentHere(agent::Kind kind);
    // The shown agent session's kind, else the first installed kind.
    agent::Kind defaultAgentKind();
    void openNewMenu();
    // The session menu of row `id` (sidebar secondary click, caption menu button) at a scene point.
    void openRowMenu(std::uint64_t id, float sceneX, float sceneY);
    void renameSession(std::uint64_t id);
    void duplicateSession(std::uint64_t id);
    // Closes `ids`; asks once first when a command runs in any of them.
    void askClose(std::vector<std::uint64_t> ids);
    void askPaste(const std::string& text, std::function<void()> paste);
    // The window title: the session's own name, else the agent's title or the cwd.
    void syncTitle();
    ContentView* shownView();
    TerminalView* shownTerminal();

    AppLoop& m_loop;
    AppWindow& m_window;
    Notifier& m_notifier;
    AgentHosts m_agents;
    ThumbnailService& m_thumbnails;
    SettingsStore& m_settings;
    int m_zoom = 0; // text size steps (px at ui_scale 1) added to the setting in this window
    Signal<>::ScopedConnection m_settingsChanged;
    std::function<void()> m_close;
    // A session opened, closed, was shown, renamed, joined or left a split, or changed folder or kind.
    std::function<void(LayoutSave)> m_sessionsChanged;
    std::function<void(const std::string& token, std::vector<WindowOrigin> origins)> m_newWindow;
    std::function<void(const std::string& token)> m_preferences;
    ChooseSaveFile m_chooseSaveFile;
    // A copy of a session's screen and scrollback: in a pager session (Ctrl+Shift+H), or to a file the user picks.
    void showScrollback(std::uint64_t id);
    void saveScrollback(std::uint64_t id);
    SessionList m_sessions;
    static inline std::uint32_t s_nextWindowId = 1;
    const std::uint32_t m_windowId = s_nextWindowId++;
    std::string m_filter; // the sidebar search text
    Input* m_search = nullptr;
    bool m_rail = false; // the sidebar is an icon rail
    Flex* m_agentBar = nullptr; // title band pills of the shown agent session
    void syncAgentBar();
    void fillAgentBar();
    [[nodiscard]] AgentView* agentView(std::uint64_t key);
    Button* m_newButton = nullptr; // the sidebar "+": its menu opens new sessions and windows
    SelectionList* m_rowList = nullptr;
    Box* m_dropLine = nullptr; // where a dragged row lands in the sidebar order
    ScrollViewState m_sidebarScroll; // the row list's offset across sidebar rebuilds
    std::uint64_t m_tintTarget = 0; // the row the tint moves to
    std::uint64_t m_rowSelected = 0; // the row with selected labels: it changes at half the tint's travel
    std::vector<std::pair<std::uint64_t, StatusRow*>> m_rows; // session id, its sidebar row
    std::vector<std::pair<std::uint64_t, Flex*>> m_groups;    // a split's first session, its sidebar block
    // Session ids as the sidebar lists them: a split's members together, in pane order.
    [[nodiscard]] std::vector<std::uint64_t> sidebarOrder() const;
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
    void setFindCount(std::size_t total, std::optional<std::size_t> current, FindError error);
    void toggleFindOption(bool regex);
    FindOptions m_findOptions;
    std::array<Button*, 2> m_findToggles{}; // match case, regex
    bool m_findOpen = false;
    std::string m_findQuery;
    Node* m_findBar = nullptr;
    Input* m_findInput = nullptr;
    Label* m_findCount = nullptr;
    std::array<Button*, 2> m_findSteps{}; // prev, next
    std::vector<Session> m_views;
    // Closed sessions whose nodes are still in the scene: freed after the next build replaced them.
    std::vector<Session> m_retired;
    // Session ids are unique in the app: notice keys use them, and a moved session keeps its id.
    static inline std::uint64_t s_nextId = 1;
    void registerDropTarget();
    wl_surface* m_dropSurface = nullptr; // the surface the drop target is on
    // Agent session keys are unique in the app: all windows share one host.
    static inline std::uint64_t s_nextAgentKey = 0;
    // The panes whose views are in the slot, in layout order: the shown session alone, or its split.
    std::vector<std::uint64_t> m_builtPanes;
    Node* m_contentSlot = nullptr; // holds the visible views' nodes
    float m_contentWidth = 0.0F;
    float m_contentHeight = 0.0F;
    void fillSlot(Renderer& renderer);
    // One card per pane while a split shows, each with its caption over the pane's view.
    using PaneRect = ui::PaneRect;
    [[nodiscard]] SplitView::Callbacks splitCallbacks();
    SplitView m_split;
    // A card's symbol and spinner (its caption lead), per session.
    struct CaptionLead {
      Glyph* symbol = nullptr;
      Spinner* spinner = nullptr;
    };
    std::unordered_map<std::uint64_t, CaptionLead> m_captionLeads;
    // The smallest pane: split_min_columns x split_min_rows cells of the font, with the grid's insets.
    [[nodiscard]] std::pair<float, float> paneMinimum() const;
    // A sidebar row dragged onto a pane: the half under the pointer previews where it joins.
    [[nodiscard]] std::optional<ui::SplitDrop> dropTarget(std::uint64_t dragged, float sceneX, float sceneY) const;
    void rowDrag(std::uint64_t id, StatusRow::DragPhase phase, float sceneX, float sceneY);
    [[nodiscard]] bool leavesGroup(std::uint64_t id, float sceneX, float sceneY) const;
    // The session a row dropped at sceneY lands before in sidebar order (0: the end); nullopt off the list.
    [[nodiscard]] std::optional<std::uint64_t> dropBefore(std::uint64_t dragged, float sceneX, float sceneY) const;
    void showDropLine(std::optional<std::uint64_t> before);
    [[nodiscard]] std::optional<std::uint64_t> blockOf(const Node* child) const; // a sidebar block's session
    bool m_dragGhost = false; // the dragged row's chip follows the pointer
    bool m_rebuildPending = false; // a window rebuild fills the slot: show() leaves it alone
    // Captions follow their sessions; true when a name or state text changed (the captions need a fit).
    bool refreshCards();
    // Focuses the nearest pane in direction (dx, dy); false without a split.
    bool focusPane(float dx, float dy);
    // The shown pane trades places with its neighbour that way; false without a split (the keys go to the program).
    bool movePane(float dx, float dy);
    // A new shell joins the shown session on `edge` (a TileEdge); nothing when its split is full.
    void splitNew(std::uint32_t edge);
    bool m_active = false;
    Surface* m_tickSurface = nullptr;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace term
