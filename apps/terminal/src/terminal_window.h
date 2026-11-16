#pragma once

#include "settings_store.h"
#include "core/timer_manager.h"
#include "session_list.h"
#include "ui/controls/status_row.h"
#include "terminal_view.h"
#include "agent_view.h"
#include "split_trees.h"

#include <array>
#include <map>
#include <cstdint>
#include <optional>
#include <unordered_set>
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

namespace term {

  class Notifier;
  namespace agent {
    class Host;
  }

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
    std::string cwd;
    std::string prompt; // sent as the first message; empty: none
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
                   std::function<void()> close, std::function<void(const std::string& token)> newWindow,
                   std::function<void(const std::string& token)> preferences);
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
    void updateSceneState(); // scene busy, secret and remote follow the visible panes
    std::unordered_set<std::uint64_t> m_secretSessions; // sessions at a password prompt
    std::unordered_set<std::uint64_t> m_remoteSessions; // sessions on another host (SSH, container)
    void show(std::uint64_t id);
    // Session keys: Ctrl+Shift+T new, Ctrl+Shift+A new agent session, Ctrl+Shift+W close an agent session,
    // Ctrl+Shift+N new window, Ctrl+Tab / Ctrl+Shift+Tab recent, Ctrl+1..9 by position.
    bool sessionKey(const KeyboardEvent& event);
    void openAgentHere();
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
    agent::Host& m_agents;
    ThumbnailService& m_thumbnails;
    SettingsStore& m_settings;
    Signal<>::ScopedConnection m_settingsChanged;
    std::function<void()> m_close;
    std::function<void(const std::string& token)> m_newWindow;
    std::function<void(const std::string& token)> m_preferences;
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
    // The panes whose views are in the slot, in layout order: the shown session alone, or its split.
    std::vector<std::uint64_t> m_builtPanes;
    Node* m_contentSlot = nullptr; // holds the visible views' nodes
    float m_contentWidth = 0.0F;
    float m_contentHeight = 0.0F;
    void fillSlot(Renderer& renderer);
    // A split: one card per pane, each with its caption over the pane's view.
    struct PaneRect {
      float x = 0.0F;
      float y = 0.0F;
      float width = 0.0F;
      float height = 0.0F;
    };
    struct PaneCard {
      std::uint64_t id = 0;
      PaneRect rect;
      Flex* cell = nullptr;
      InputArea* captionArea = nullptr;
      float textWidth = 0.0F; // caption room for the name and the state
      Box* stroke = nullptr;  // the card's edge, over its view (a view fills its whole rect)
      Box* ring = nullptr;    // the focus ring over the stroke: opacity 1 on the focused card
      std::uint32_t focusAnim = 0;
      Box* shadow = nullptr;
      Glyph* symbol = nullptr;
      Spinner* spinner = nullptr;
      Label* name = nullptr;
      Label* state = nullptr;
      Flex* accessory = nullptr; // holds the find and agent bars while the pane is focused
      bool topRight = false;     // holds the window buttons
    };
    std::vector<PaneCard> m_cards; // empty while one session shows
    // Each split lays out on the compositor's dwindle tree; its leaves are session ids.
    using TileNode = andromeda::tiling::DwindleTree::Node;
    SplitTrees m_trees;
    // The smallest pane: split_min_columns x split_min_rows cells of the font, with the grid's insets.
    [[nodiscard]] std::pair<float, float> paneMinimum() const;
    [[nodiscard]] SplitTrees::Room splitRoom() const;
    struct Divider {
      bool vertical = true; // between columns
      TileNode* split = nullptr;
      InputArea* area = nullptr;
      Box* grip = nullptr;
    };
    std::vector<Divider> m_dividers;
    std::optional<std::size_t> m_dragDivider; // the divider under a held button; views keep their size until release
    [[nodiscard]] std::vector<PaneRect> paneRects(const std::vector<std::uint64_t>& panes);
    [[nodiscard]] float captionTextWidth(float width, bool topRight) const;
    // Cards follow the tree in place; settle: the views take their new sizes (one PTY resize).
    void relayoutSplit(bool settle);
    void placeDividers();
    void dragDivider(std::size_t index, float sceneX, float sceneY);
    // A sidebar row dragged onto a pane: the half under the pointer previews where it joins.
    struct DropTarget {
      std::uint64_t target = 0;
      bool before = false; // the left or top half
      std::uint32_t edge = 0; // the TileEdge of that half
      PaneRect rect;       // in the slot
    };
    [[nodiscard]] std::optional<DropTarget> dropTarget(std::uint64_t dragged, float sceneX, float sceneY) const;
    void rowDrag(std::uint64_t id, StatusRow::DragPhase phase, float sceneX, float sceneY);
    Box* m_dropPreview = nullptr;
    // A closed pane's place and fill: the next build fades a plate out there, under the cards that take its room.
    struct ClosedCard {
      PaneRect rect;
      ColorSpec fill;
    };
    std::optional<ClosedCard> m_closedCard;
    bool m_dragGhost = false; // the dragged row's chip follows the pointer
    std::uint64_t m_ringPane = 0;  // the card that draws the focus ring
    // from: where cards were before this build (a pane's old card, the single view, the drop preview); they move to
    // their new places on the move spring, a card with no old place fades in.
    void buildSplit(Renderer& renderer, const std::vector<std::uint64_t>& panes,
                    const std::vector<std::pair<std::uint64_t, PaneRect>>& from);
    // Cells, rings and caption areas at these rects; card.rect (the views' size) stays.
    void applyCellRects(const std::vector<PaneRect>& rects);
    std::uint32_t m_layoutAnim = 0;
    std::uint64_t m_slotBuild = 0; // counts fillSlot calls
    bool m_rebuildPending = false; // a window rebuild fills the slot: show() leaves it alone
    // Runs `start` with the slot's animation manager: now, or after a full rebuild puts the slot in the scene (if no
    // other build came first).
    void whenAnimating(std::function<void(AnimationManager&)> start);
    std::optional<std::pair<std::uint64_t, PaneRect>> m_dropFrom; // the joining session's preview rect
    // Ring, shadow, caption weight and the find and agent bars follow the shown session, in place.
    void applyPaneFocus();
    // animate: ring and shadow fade (in on fade, out on fade_out); else they jump.
    void paintPaneFocus(bool animate);
    // Captions follow their sessions; true when a name or state text changed (the captions need a fit).
    bool refreshCards();
    void fitCaptions(Renderer& renderer);
    // Ctrl+Alt+arrow: the nearest pane in that direction; false when the key is not one.
    bool focusPaneKey(const KeyboardEvent& event);
    bool m_active = false;
    Surface* m_tickSurface = nullptr;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace term
