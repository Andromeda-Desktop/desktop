#pragma once

#include "content_view.h"
#include "context_row.h"
#include "core/timer_manager.h"
#include "pty_session.h"
#include "session_list.h"
#include "settings.h"
#include "theme_store.h"
#include "terminal_find.h"
#include "render/core/color.h"
#include "ui/palette.h"
#include "ui/theme_palette.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/terminal_grid_node.h"
#include "design_tokens.h"
#include "ui/text_input_client.h"
#include "ui/tooltip_content.h"

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
class ThumbnailService;

namespace term {

  class KittyImages;

  // The shell runs on another host: an ssh-like command or a container shell, or a remote OSC 7.
  struct Remote {
    enum class Kind : std::uint8_t { Ssh, Container };
    Kind kind = Kind::Ssh;
    std::optional<std::string> host; // empty until the command line or a remote OSC 7 names it
  };

  // file://host/path (percent-escaped) to a local path; a bare path comes back as is.
  [[nodiscard]] std::string pathFromFileUri(std::string_view uri);
  // A colour role under a terminal theme: System follows the desktop scheme, AMOLED is Dark on pure black.
  [[nodiscard]] Color themeColor(Theme theme, ColorRole role);
  // A built-in theme or an installed package; a package follows the desktop's light or dark scheme.
  using ThemeSpec = std::variant<Theme, ThemePackage>;
  // The grid colours of a terminal theme, derived from its view, label and accent anchors.
  [[nodiscard]] ThemePalette themePalette(const ThemeSpec& theme);
  // The theme an id names; an unknown id falls back to System.
  [[nodiscard]] ThemeSpec resolveTheme(std::string_view id, const ThemeStore& themes);
  [[nodiscard]] inline bool hasScene(const ThemeSpec& theme) {
    const auto* package = std::get_if<ThemePackage>(&theme);
    return package != nullptr && !std::holds_alternative<std::monostate>(package->backdrop);
  }
  // The grid font family list: the user's font, else the theme's, then the mono token.
  [[nodiscard]] std::string gridFont(const Settings& settings, const ThemeSpec& theme);

  // One terminal session drawn as a libdesk-ui cell grid. The app window and the Files pane both host it.
  // All methods run on the UI thread; the session's IO thread only wakes it.
  class TerminalView final : public ContentView, private TextInputClient {
  public:
    struct Host {
      // New terminal state is ready: ask for a frame tick that calls frame().
      std::function<void()> requestFrame;
      std::function<void()> playBell; // the bell setting is Sound
      std::function<void()> bellRang; // any bell setting but Off, once per bell interval
      std::function<void(bool)> secretInput; // the program reads a password (echo off), or stopped
      // Null until GL is attached: the scene builds before EGL is up.
      std::function<TextureManager*()> textures;
      // Image thumbnails for the link hover preview; null shows text only.
      ThumbnailService* thumbnails = nullptr;
      std::function<void(int status)> exited;
      // Current keyboard modifiers (KeyMod bits): pointer events carry none.
      std::function<std::uint32_t()> modifiers;
      // Post a desktop notification: a long command finished while the grid had no focus (OSC 133), or the
      // program asked (OSC 9 / 777 / 99). activated gets the clicked button (0: the body); either may be empty.
      std::function<void(const Notice& notice, std::function<void(int button)> activated, std::function<void()> closed)>
          notify;
      std::function<void(const std::string& key)> closeNotice; // OSC 99 p=close for a Notice::key
      // A paste with a newline into a program without bracketed paste: run paste after the user agrees.
      std::function<void(const std::string& text, std::function<void()> paste)> confirmPaste;
      // The program set its title (OSC 0 / 2).
      std::function<void(std::string title)> titleChanged;
      // OSC 133: a command started running / ended (exit code when the shell reports one).
      std::function<void(const std::string& command)> commandStarted;
      std::function<void(std::optional<int> exitCode)> commandEnded;
      // The running command started or stopped printing (CommandOutput: Active / Quiet).
      std::function<void(CommandOutput output)> commandOutput;
      // The shell reported a new working directory (OSC 7); a local path.
      std::function<void(const std::string& cwd)> cwdChanged;
      // The shell moved to another host (remote: none = this machine); moved is false when only the host name was learned.
      std::function<void(const std::optional<Remote>& remote, bool moved)> hostChanged;
      // Ctrl+click on a link: an http(s), mailto or file URI.
      std::function<void(const std::string& uri)> openUri;
      // Find results changed: match count and the current match (1 = newest), while find is open.
      std::function<void(std::size_t total, std::optional<std::size_t> current, FindError error)> findChanged;
      // The alpha of the backing behind text over a theme scene (AppWindow::sceneTextBacking); null = none.
      std::function<float()> textBacking;
      Settings settings;
      ThemeSpec theme = Theme::System;
    };

    explicit TerminalView(Host host);
    ~TerminalView() override;
    TerminalView(const TerminalView&) = delete;
    TerminalView& operator=(const TerminalView&) = delete;

    bool start(std::vector<std::string> argv, std::string cwd, const std::vector<std::string>& extraEnv = {});
    // Takes over a session whose terminal process died; false when the holder no longer has it.
    bool reattach(std::uint64_t heldId);

    // uiScale: logical px per design px; deviceScale: buffer px per logical px. The GL context is current.
    std::unique_ptr<Node> build(Renderer& renderer, float uiScale, float deviceScale, float width, float height) override;
    // The view got a new size (logical px): resizes the grid and the PTY.
    void setSize(float width, float height) override;
    // Pulls the terminal state into the grid. Call with the GL context current, once per frame tick.
    void frame() override;
    // Preferences changed: font, theme, cursor, scrolling and notifications apply at once.
    void applySettings(const Settings& settings, const ThemeSpec& theme);
    // The background as the grid draws it, with its alpha.
    [[nodiscard]] ColorSpec surface() const override;
    [[nodiscard]] float backgroundAlpha() const;
    // The shell's working directory (OSC 7); empty until it reports one.
    [[nodiscard]] const std::string& cwd() const noexcept { return m_cwd; }
    // One cell in logical px; 0 before the grid is built.
    [[nodiscard]] float cellWidth() const noexcept;
    [[nodiscard]] float cellHeight() const noexcept;
    // Its nodes were freed with the window's scene: until the next build the view has none, as before the first.
    void sceneDestroyed() override {
      if (m_bellAnim != 0 && m_area != nullptr && m_area->animationManager() != nullptr) {
        m_area->animationManager()->cancel(m_bellAnim); // its callbacks touch m_bellFlash, freed with the scene
      }
      m_bellAnim = 0;
      m_bellFlash = nullptr;
      m_contextTick.stop(); // it reads the grid; the next build's syncContext starts it again
      m_grid = nullptr;
      m_area = nullptr;
      m_linkAnchor = nullptr;
    }
    // The output buffer scale changed: the next frame reloads the glyph atlas at this scale.
    void setDeviceScale(float deviceScale) override;

    // A key for the program, or Ctrl+Shift+C/V (copy the selection, paste); false when the window should handle
    // it (Super, other Ctrl+Shift chords, keys pressed before the grid had focus).
    bool key(const KeyboardEvent& event) override;
    // The window gained or lost activation. Focus = the grid has keyboard focus in an active window: a solid
    // cursor and focus reports (mode 1004); otherwise a hollow cursor.
    void setWindowActive(bool active) override;
    // Find over the screen and scrollback; an empty query clears the hits.
    void setFindQuery(const std::string& query, FindOptions options);
    // older: toward history (Enter); else toward the prompt (Shift+Enter).
    void findStep(bool older);
    void closeFind();
    // A drop on the grid: text/uri-list as shell-quoted paths, any other type as text, both through paste.
    void pasteDrop(const std::string& mime, const std::string& data);

    [[nodiscard]] InputArea* focusArea() const noexcept override { return m_area; }
    [[nodiscard]] TerminalView* terminal() noexcept override { return this; }
    [[nodiscard]] PtySession& session() noexcept { return m_session; }

  private:
    // Redraw owed to the next frame tick: Dirty rows only, or All rows (the selection changed).
    enum class Redraw : std::uint8_t { None, Dirty, All };
    struct DragIdle {};
    struct DragSelecting {
      GhosttyPointCoordinate anchor; // viewport cell where the drag started
      bool moved = false;
      std::optional<std::string> link; // URI under a single press; a release without a drag opens it
    };
    struct CommandIdle {};
    struct CommandRunning {
      std::chrono::steady_clock::time_point start;
      std::string text;
      std::chrono::steady_clock::time_point lastOutput;
      CommandOutput output = CommandOutput::Active;
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
    void noteCommandOutput();
    void armQuiet(std::chrono::milliseconds delay);
    void onBell();
    void updateCwd();
    void setRemote(std::optional<Remote> remote);
    [[nodiscard]] GhosttyPointCoordinate cellAt(float localX, float localY) const;
    void pointerButton(std::uint32_t button, float localX, float localY, bool pressed, std::uint32_t time);
    void pointerMotion(float localX, float localY);
    bool wheel(const InputArea::PointerData& d);
    [[nodiscard]] bool mouseTracking();
    struct ContextKey {
      std::uint64_t generation = 0;
      std::uint64_t top = 0;
      int rows = 0;
      GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
      std::uint16_t cols = 0;
      bool operator==(const ContextKey&) const = default;
    };
    ContextKey contextKey();
    void syncContext();
    void readBranch(const void* prompt, std::string cwdUri, std::string path);
    // Rows of the viewport; the grid holds one more, the overscan row a pixel scroll shows below them.
    [[nodiscard]] int viewRows() const noexcept { return m_grid->rows() - 1; }
    // Scrolls by the viewport height less one row; negative pages go back into the scrollback.
    void scrollPage(int pages);
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
    void updateLink(bool ctrl, bool rescan = false);
    void clearLink();
    void applyLink(std::optional<HoverLink> next);

    // IME: commits go to the program; the preedit is drawn over the grid at the cursor.
    [[nodiscard]] TextInputState textInputState() const override;
    void textInputApplyEdit(const TextInputEdit& edit) override;
    void textInputResetPreedit() override;
    void textInputActivated(TextInputService& service) override;
    void textInputDeactivated(TextInputService& service) override;
    void programNotice(const Notice& notice);
    std::string selectionText();
    static std::string formatText(GhosttyTerminal t, const GhosttySelection& selection);
    void jumpPrompt(int direction);
    void copyLastOutput();
    void copySelection();
    void publishSelection();
    void paste();
    void pasteText(std::string text);
    void writePaste(std::string text, bool bracketed);

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
    float m_reportPx = 0.0F; // finger travel not yet sent as wheel reports
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
    std::optional<Remote> m_remote; // none = this machine
    std::optional<HoverLink> m_link;
    bool m_overPill = false; // the pointer is on a context-row pill
    struct LinkProbe {
      GhosttyPointCoordinate cell;
      bool ctrl;
    };
    std::optional<LinkProbe> m_linkProbe; // the input m_link was computed from
    Settings m_settings = m_host.settings;
    ThemeSpec m_theme = m_host.theme;
    void applyTerminalSettings();
    [[nodiscard]] Color themed(ColorRole role) const;
    void setGridFont();
    // Hover preview of m_link: Armed waits the tooltip delay; Shown holds the content and, for an image, the
    // thumbnail it acquired (thumbPath; released when the preview leaves Shown).
    struct PreviewOff {};
    struct PreviewArmed {};
    struct PreviewShown {
      TooltipPreview content;
      std::string thumbPath;
      int thumbPx = 0;
    };
    using Preview = std::variant<PreviewOff, PreviewArmed, PreviewShown>;
    Preview m_preview;
    Timer m_previewTimer;
    Timer m_quietTimer; // one shot at the running command's quiet deadline
    std::shared_ptr<void> m_thumbWake;
    Node* m_linkAnchor = nullptr; // the tooltip anchor over the link span
    void setPreview(Preview next);
    void showPreview();
    [[nodiscard]] int previewPx() const;
    TextInputService* m_textInput = nullptr;
    std::string m_preedit;
    GhosttyPointCoordinate m_cursorCell{}; // viewport cell the IME candidate window follows
    std::unordered_set<std::uint32_t> m_keysDown; // evdev codes whose press went to the program
    void updateFocus();
    void updateBlink(bool wanted);
    void restartBlink();

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
    std::optional<ContextKey> m_contextKey; // what the shown pills were placed for
    std::vector<std::uint16_t> m_contextRows; // viewport rows of the prompts in view at that time
    // Steady and Expired: the cursor shows and no timer runs; Expired = blinking timed out, a key restarts it.
    enum class Blink : std::uint8_t { Steady, Shown, Hidden, Expired };
    Blink m_blink = Blink::Steady;
    std::optional<TerminalCursor> m_cursor; // from the last refresh; the blink tick shows or hides it
    Timer m_blinkTimer;
    std::chrono::steady_clock::time_point m_blinkSince; // the last key or focus gain
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
    std::uint32_t m_cursorColor = 0; // the theme accent; a program's OSC 12 colour wins
    Color m_findHit{};
    Color m_findHitCurrent{};
    struct FindClosed {};
    std::variant<FindClosed, TerminalFind> m_find; // destroyed under the terminal lock
    Timer m_findRescan; // one shot: the frame that rescans a changed screen for a regex or case-sensitive query
    static constexpr std::chrono::milliseconds kFindRescan{250};
    bool m_marksDirty = false;
    using CommandMarksKey = std::pair<std::size_t, std::optional<std::uint32_t>>; // entries, oldest prompt row
    CommandMarksKey m_commandMarksKey;
    Node* m_findLayer = nullptr; // moves with the grid origin
    RectNode* m_findRing = nullptr;
    RectNode* m_bellFlash = nullptr; // the bell line along the title band's bottom edge
    AnimationManager::Id m_bellAnim = 0;
    std::chrono::steady_clock::time_point m_lastBell;
    void updateMarks();
    std::vector<TerminalCell> m_rowBuffer;
    bool m_exitReported = false;
  };

} // namespace term
