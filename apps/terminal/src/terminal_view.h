#pragma once

#include "context_row.h"
#include "core/timer_manager.h"
#include "pty_session.h"
#include "terminal_find.h"
#include "render/core/color.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/terminal_grid_node.h"
#include "design_tokens.h"
#include "ui/text_input_client.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

class InputArea;
class Node;
class Renderer;
class RectNode;
class Scrollbar;
struct KeyboardEvent;
class TextureManager;

namespace term {

  class KittyImages;

  // file://host/path (percent-escaped) to a local path; a bare path comes back as is.
  [[nodiscard]] std::string pathFromFileUri(std::string_view uri);

  // One terminal session drawn as a libdesk-ui cell grid. The app window and the Files pane both host it.
  // All methods run on the UI thread; the session's IO thread only wakes it.
  class TerminalView final : private TextInputClient {
  public:
    struct Host {
      // New terminal state is ready: ask for a frame tick that calls frame().
      std::function<void()> requestFrame;
      // Null until GL is attached: the scene builds before EGL is up.
      std::function<TextureManager*()> textures;
      std::function<void(int status)> exited;
      // Current keyboard modifiers (KeyMod bits): pointer events carry none.
      std::function<std::uint32_t()> modifiers;
      // A long command finished while the grid had no focus (OSC 133): post a notification.
      std::function<void(const std::string& summary, const std::string& body)> commandFinished;
      // OSC 133: a command started running / ended (exit code when the shell reports one).
      std::function<void(const std::string& command)> commandStarted;
      std::function<void(std::optional<int> exitCode)> commandEnded;
      // The shell reported a new working directory (OSC 7); a local path.
      std::function<void(const std::string& cwd)> cwdChanged;
      // Ctrl+click on a link: an http(s), mailto or file URI.
      std::function<void(const std::string& uri)> openUri;
      // Find results changed: match count and the current match (1 = newest), while find is open.
      std::function<void(std::size_t total, std::optional<std::size_t> current)> findChanged;
    };

    explicit TerminalView(Host host);
    ~TerminalView() override;
    TerminalView(const TerminalView&) = delete;
    TerminalView& operator=(const TerminalView&) = delete;

    bool start(std::vector<std::string> argv, std::string cwd, const std::vector<std::string>& extraEnv = {});
    // Takes over a session whose terminal process died; false when the holder no longer has it.
    bool reattach(std::uint64_t heldId);

    // uiScale: logical px per design px; deviceScale: buffer px per logical px. The GL context is current.
    std::unique_ptr<Node> build(Renderer& renderer, float uiScale, float deviceScale, float width, float height);
    // The view got a new size (logical px): resizes the grid and the PTY.
    void setSize(float width, float height);
    // Pulls the terminal state into the grid. Call with the GL context current, once per frame tick.
    void frame();
    // Its nodes were freed with the window's scene: until the next build the view has none, as before the first.
    void sceneDestroyed() {
      m_grid = nullptr;
      m_area = nullptr;
    }
    // The output buffer scale changed: the next frame reloads the glyph atlas at this scale.
    void setDeviceScale(float deviceScale);

    // A key for the program, or Ctrl+Shift+C/V (copy the selection, paste); false when the window should handle
    // it (Super, other Ctrl+Shift chords, keys pressed before the grid had focus).
    bool key(const KeyboardEvent& event);
    // The window gained or lost activation. Focus = the grid has keyboard focus in an active window: a solid
    // cursor and focus reports (mode 1004); otherwise a hollow cursor.
    void setWindowActive(bool active);
    // Find over the screen and scrollback; an empty query clears the hits.
    void setFindQuery(const std::string& query);
    // older: toward history (Enter); else toward the prompt (Shift+Enter).
    void findStep(bool older);
    void closeFind();

    [[nodiscard]] InputArea* focusArea() const noexcept { return m_area; }
    [[nodiscard]] PtySession& session() noexcept { return m_session; }

  private:
    // Redraw owed to the next frame tick: Dirty rows only, or All rows (the selection changed).
    enum class Redraw : std::uint8_t { None, Dirty, All };
    struct DragIdle {};
    struct DragSelecting {
      GhosttyPointCoordinate anchor; // viewport cell where the drag started
      bool moved = false;
    };
    struct CommandIdle {};
    struct CommandRunning {
      std::chrono::steady_clock::time_point start;
      std::string text;
    };
    struct Click {
      std::uint32_t time = 0; // ms, Wayland event time
      GhosttyPointCoordinate cell{};
      int count = 0; // 1 cell, 2 word, 3 line
    };

    void refresh(bool all);
    // Writes the preedit, underlined, into m_rowBuffer from column x; returns the columns it covers.
    int overlayPreedit(int x, std::uint32_t fg, std::uint32_t bg);
    void applyPalette();
    void scheduleRedraw(Redraw redraw);
    void setGridOrigin(float x, float y);
    void onMark(const CommandMark& mark);
    void updateCwd();
    [[nodiscard]] GhosttyPointCoordinate cellAt(float localX, float localY) const;
    void pointerButton(std::uint32_t button, float localX, float localY, bool pressed, std::uint32_t time);
    void pointerMotion(float localX, float localY);
    bool wheel(const InputArea::PointerData& d);
    [[nodiscard]] bool mouseTracking();
    void syncContext();
    void readBranch(const void* prompt, std::string cwdUri, std::string path);
    // Rows of the viewport; the grid holds one more, the overscan row a pixel scroll shows below them.
    [[nodiscard]] int viewRows() const noexcept { return m_grid->rows() - 1; }
    // Viewport top in px from the top of the scrollback: ghostty scrolls whole rows, the grid origin the remainder.
    void setScrollPx(float px);
    void animateScroll(float target, const Tokens::Spring& spring, float velocity, bool spatial);
    void settleScroll();
    [[nodiscard]] float fingerVelocity() const;
    // Sends the event to the program when it tracks the mouse; false when it does not.
    bool reportMouse(GhosttyMouseAction action, GhosttyMouseButton button, float localX, float localY,
                     std::uint32_t mods);
    void selectAround(GhosttyPointCoordinate cell, int count);
    void clearSelection();
    void scrollToBottom();
    [[nodiscard]] std::uint32_t modifiers() const;
    // Ctrl+hover: the link under the pointer gets an underline and the pointer cursor.
    struct HoverLink {
      std::uint32_t row = 0; // viewport row
      int start = 0;         // columns [start, end)
      int end = 0;
      std::string uri;
      bool operator==(const HoverLink&) const = default;
    };
    [[nodiscard]] std::optional<HoverLink> linkAt(GhosttyPointCoordinate cell);
    void updateLink(bool ctrl);

    // IME: commits go to the program; the preedit is drawn over the grid at the cursor.
    [[nodiscard]] TextInputState textInputState() const override;
    void textInputApplyEdit(const TextInputEdit& edit) override;
    void textInputResetPreedit() override;
    void textInputActivated(TextInputService& service) override;
    void textInputDeactivated(TextInputService& service) override;
    void copySelection();
    void paste();

    Host m_host;
    void commitAtlas();
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    PtySession m_session;
    GhosttyRenderState m_state = nullptr;
    GhosttyRenderStateRowIterator m_rows = nullptr;
    GhosttyRenderStateRowCells m_cells = nullptr;
    GhosttyKeyEncoder m_encoder = nullptr;
    GhosttyKeyEvent m_keyEvent = nullptr;
    GhosttyMouseEncoder m_mouseEncoder = nullptr;
    GhosttyMouseEvent m_mouseEvent = nullptr;
    std::uint32_t m_buttonsReported = 0; // bit per GhosttyMouseButton whose press went to the program
    GhosttyPointCoordinate m_motionCell{}; // last cell a motion report went out for
    float m_pointerX = 0.0F; // last pointer position, local px: wheel reports need one
    float m_pointerY = 0.0F;
    float m_wheelRemainder = 0.0F;
    // Rest: the view sits on a whole row and follows ghostty. Finger: a touchpad tracks 1:1. Settling: a spring
    // (wheel notch or fling) runs toward m_scrollTarget, a whole row.
    enum class ScrollMotion : std::uint8_t { Rest, Finger, Settling };
    struct ScrollSample {
      std::uint32_t time; // ms
      float px;
    };
    ScrollMotion m_scrollMotion = ScrollMotion::Rest;
    float m_scrollPx = 0.0F;
    float m_scrollTarget = 0.0F;
    std::uint64_t m_viewRow = 0; // the viewport row ghostty was last scrolled to
    AnimationManager::Id m_scrollAnim = 0;
    std::vector<ScrollSample> m_scrollSamples; // finger positions of the last 100 ms (rulebook 7.7)
    Click m_lastClick;
    std::variant<CommandIdle, CommandRunning> m_command;
    std::string m_cwd;
    std::optional<HoverLink> m_link;
    bool m_overPill = false; // the pointer is on a context-row pill
    TextInputService* m_textInput = nullptr;
    std::string m_preedit;
    GhosttyPointCoordinate m_cursorCell{}; // viewport cell the IME candidate window follows
    std::unordered_set<std::uint32_t> m_keysDown; // evdev codes whose press went to the program
    void updateFocus();

    bool m_windowActive = true;
    bool m_areaFocused = false;
    bool m_focused = false;
    Redraw m_redraw = Redraw::None;
    std::variant<DragIdle, DragSelecting> m_drag;

    InputArea* m_area = nullptr;
    TerminalGridNode* m_grid = nullptr;
    std::unique_ptr<KittyImages> m_images;
    std::unique_ptr<ContextRow> m_context;
    Timer m_contextTick; // 1 Hz, only while a shown prompt's command runs
    std::array<std::uint16_t, 2> m_cellPx{}; // cell size the terminal last got, device px
    Scrollbar* m_scrollbar = nullptr;
    GhosttyTerminalScrollbar m_scrollState{}; // rows; the bar is updated only when this changes
    float m_uiScale = 1.0F;
    float m_deviceScale = 1.0F; // buffer px per logical px the atlas was loaded for
    float m_pendingDeviceScale = 1.0F;
    float m_width = 0.0F;
    float m_height = 0.0F;
    std::uint32_t m_foreground = 0;
    std::uint32_t m_faint = 0;
    std::uint32_t m_background = 0;
    std::uint32_t m_selection = 0;
    Color m_findHit{};
    Color m_findHitCurrent{};
    struct FindClosed {};
    std::variant<FindClosed, TerminalFind> m_find; // destroyed under the terminal lock
    bool m_marksDirty = false;
    using CommandMarksKey = std::pair<std::size_t, std::optional<std::uint32_t>>; // entries, oldest prompt row
    CommandMarksKey m_commandMarksKey;
    Node* m_findLayer = nullptr; // moves with the grid origin
    RectNode* m_findRing = nullptr;
    void updateMarks();
    std::vector<TerminalCell> m_rowBuffer;
    bool m_exitReported = false;
  };

} // namespace term
