#pragma once

#include "pty_session.h"
#include "render/scene/terminal_grid_node.h"
#include "ui/text_input_client.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_set>
#include <variant>
#include <vector>

class InputArea;
class Node;
class Renderer;
class Scrollbar;
struct KeyboardEvent;
class TextureManager;

namespace term {

  // One terminal session drawn as a libdesk-ui cell grid. The app window and the Files pane both host it.
  // All methods run on the UI thread; the session's IO thread only wakes it.
  class TerminalView final : private TextInputClient {
  public:
    struct Host {
      // New terminal state is ready: ask for a frame tick that calls frame().
      std::function<void()> requestFrame;
      std::function<void(int status)> exited;
      // Current keyboard modifiers (KeyMod bits): pointer events carry none.
      std::function<std::uint32_t()> modifiers;
    };

    explicit TerminalView(Host host);
    ~TerminalView() override;
    TerminalView(const TerminalView&) = delete;
    TerminalView& operator=(const TerminalView&) = delete;

    bool start(std::vector<std::string> argv, std::string cwd);

    // uiScale: logical px per design px; deviceScale: buffer px per logical px. The GL context is current.
    std::unique_ptr<Node> build(Renderer& renderer, float uiScale, float deviceScale, float width, float height);
    // The view got a new size (logical px): resizes the grid and the PTY.
    void setSize(float width, float height);
    // Pulls the terminal state into the grid. Call with the GL context current, once per frame tick.
    void frame();
    // The output buffer scale changed: the next frame reloads the glyph atlas at this scale.
    void setDeviceScale(float deviceScale);

    // A key for the program, or Ctrl+Shift+C/V (copy the selection, paste); false when the window should handle
    // it (Super, other Ctrl+Shift chords, keys pressed before the grid had focus).
    bool key(const KeyboardEvent& event);
    // The window gained or lost activation. Focus = the grid has keyboard focus in an active window: a solid
    // cursor and focus reports (mode 1004); otherwise a hollow cursor.
    void setWindowActive(bool active);

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
    [[nodiscard]] GhosttyPointCoordinate cellAt(float localX, float localY) const;
    void pointerButton(std::uint32_t button, float localX, float localY, bool pressed, std::uint32_t time);
    void pointerMotion(float localX, float localY);
    bool wheel(std::uint32_t axis, float steps);
    // Sends the event to the program when it tracks the mouse; false when it does not.
    bool reportMouse(GhosttyMouseAction action, GhosttyMouseButton button, float localX, float localY,
                     std::uint32_t mods);
    void selectAround(GhosttyPointCoordinate cell, int count);
    void clearSelection();
    void scrollToBottom();
    [[nodiscard]] std::uint32_t modifiers() const;

    // IME: commits go to the program; the preedit is drawn over the grid at the cursor.
    [[nodiscard]] TextInputState textInputState() const override;
    void textInputApplyEdit(const TextInputEdit& edit) override;
    void textInputResetPreedit() override;
    void textInputActivated(TextInputService& service) override;
    void textInputDeactivated(TextInputService& service) override;
    void copySelection();
    void paste();

    Host m_host;
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
    Click m_lastClick;
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
    Scrollbar* m_scrollbar = nullptr;
    GhosttyTerminalScrollbar m_scrollState{}; // rows; the bar is updated only when this changes
    TextureManager* m_textures = nullptr;
    float m_uiScale = 1.0F;
    float m_deviceScale = 1.0F; // buffer px per logical px the atlas was loaded for
    float m_pendingDeviceScale = 1.0F;
    float m_width = 0.0F;
    float m_height = 0.0F;
    bool m_dark = true;
    std::uint32_t m_foreground = 0;
    std::uint32_t m_faint = 0;
    std::uint32_t m_background = 0;
    std::uint32_t m_selection = 0;
    std::vector<TerminalCell> m_rowBuffer;
    bool m_exitReported = false;
  };

} // namespace term
