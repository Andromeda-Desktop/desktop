#pragma once

#include "pty_session.h"
#include "render/scene/terminal_grid_node.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_set>
#include <vector>

class InputArea;
class Node;
class Renderer;
struct KeyboardEvent;
class TextureManager;

namespace term {

  // One terminal session drawn as a libdesk-ui cell grid. The app window and the Files pane both host it.
  // All methods run on the UI thread; the session's IO thread only wakes it.
  class TerminalView {
  public:
    struct Host {
      // New terminal state is ready: ask for a frame tick that calls frame().
      std::function<void()> requestFrame;
      std::function<void(int status)> exited;
    };

    explicit TerminalView(Host host);
    ~TerminalView();
    TerminalView(const TerminalView&) = delete;
    TerminalView& operator=(const TerminalView&) = delete;

    bool start(std::vector<std::string> argv, std::string cwd);

    // uiScale: logical px per design px; deviceScale: buffer px per logical px. The GL context is current.
    std::unique_ptr<Node> build(Renderer& renderer, float uiScale, float deviceScale, float width, float height);
    // The view got a new size (logical px): resizes the grid and the PTY.
    void setSize(float width, float height);
    // Pulls the terminal state into the grid. Call with the GL context current, once per frame tick.
    void frame();

    // A key for the program; false when the window should handle it (Super, Ctrl+Shift chords, keys pressed
    // before the grid had focus).
    bool key(const KeyboardEvent& event);
    // The window gained or lost activation. Focus = the grid has keyboard focus in an active window: a solid
    // cursor and focus reports (mode 1004); otherwise a hollow cursor.
    void setWindowActive(bool active);

    [[nodiscard]] InputArea* focusArea() const noexcept { return m_area; }
    [[nodiscard]] PtySession& session() noexcept { return m_session; }

  private:
    void refresh(bool all);
    void applyPalette();

    Host m_host;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    PtySession m_session;
    GhosttyRenderState m_state = nullptr;
    GhosttyRenderStateRowIterator m_rows = nullptr;
    GhosttyRenderStateRowCells m_cells = nullptr;
    GhosttyKeyEncoder m_encoder = nullptr;
    GhosttyKeyEvent m_keyEvent = nullptr;
    std::unordered_set<std::uint32_t> m_keysDown; // evdev codes whose press went to the program
    void updateFocus();

    bool m_windowActive = true;
    bool m_areaFocused = false;
    bool m_focused = false;
    bool m_focusChanged = false; // the cursor shape waits for the next frame

    InputArea* m_area = nullptr;
    TerminalGridNode* m_grid = nullptr;
    TextureManager* m_textures = nullptr;
    float m_uiScale = 1.0F;
    bool m_dark = true;
    std::uint32_t m_foreground = 0;
    std::uint32_t m_faint = 0;
    std::uint32_t m_background = 0;
    std::vector<TerminalCell> m_rowBuffer;
    bool m_exitReported = false;
  };

} // namespace term
