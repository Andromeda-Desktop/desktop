#include "terminal_view.h"

#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "key_map.h"
#include "wayland/wayland_seat.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "core/text_clipboard.h"
#include "ui/builders.h"
#include "ui/controls/input.h"
#include "ui/palette.h"

#include <linux/input-event-codes.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace term {

  namespace {

    // D-SPEC.md: the grid is Fira Code 13 on an 18 px line, inset 24.
    constexpr const char* kFontFamily = "Fira Code,monospace";
    constexpr float kFontSize = 13.0F;
    constexpr float kLineHeight = 18.0F;
    constexpr float kInset = 24.0F;
    constexpr float kTopInset = 8.0F;

    // D-SPEC.md theme: ANSI 1-6 at 7:1 on the view plane; the bright set (9-14) repeats them.
    constexpr std::uint32_t kAnsiLight[6] = {0xa92321, 0x146720, 0x715403, 0x2f56a1, 0x814085, 0x06626b};
    constexpr std::uint32_t kAnsiDark[6] = {0xff7c70, 0x68b56c, 0xca9d33, 0x77a4f6, 0xd28cd6, 0x03b5c3};
    constexpr std::uint32_t kViewLight = 0xffffff;
    constexpr std::uint32_t kViewDark = 0x18181b;

    std::uint32_t pack(GhosttyColorRgb c) { return (std::uint32_t(c.r) << 16) | (std::uint32_t(c.g) << 8) | c.b; }

    GhosttyColorRgb unpack(std::uint32_t rgb) {
      return {static_cast<std::uint8_t>(rgb >> 16), static_cast<std::uint8_t>(rgb >> 8), static_cast<std::uint8_t>(rgb)};
    }

    // a * (1 - t) + b * t per channel.
    std::uint32_t mix(std::uint32_t a, std::uint32_t b, float t) {
      std::uint32_t out = 0;
      for (int shift = 0; shift <= 16; shift += 8) {
        const float ca = static_cast<float>((a >> shift) & 0xFF);
        const float cb = static_cast<float>((b >> shift) & 0xFF);
        out |= static_cast<std::uint32_t>(std::lround(ca + (cb - ca) * t)) << shift;
      }
      return out;
    }

    // A translucent label token composited over the view plane.
    std::uint32_t over(const Color& c, std::uint32_t background) {
      auto byte = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F)); };
      return mix(background, (byte(c.r) << 16) | (byte(c.g) << 8) | byte(c.b), c.a);
    }

    void appendUtf8(std::string& out, std::uint32_t cp) {
      if (cp < 0x80) {
        out += static_cast<char>(cp);
      } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
      } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
      } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
      }
    }

  } // namespace

  TerminalView::TerminalView(Host host)
      : m_host(std::move(host)), m_session([this, alive = std::weak_ptr<bool>(m_alive)] {
          // IO thread: hop to the UI thread; the session latches until frame() takes the wake.
          DeferredCall::callLater([this, alive] {
            if (!alive.expired() && m_host.requestFrame) {
              m_host.requestFrame();
            }
          });
        }) {
    ghostty_render_state_new(nullptr, &m_state);
    ghostty_render_state_row_iterator_new(nullptr, &m_rows);
    ghostty_render_state_row_cells_new(nullptr, &m_cells);
    ghostty_key_encoder_new(nullptr, &m_encoder);
    ghostty_key_event_new(nullptr, &m_keyEvent);
  }

  TerminalView::~TerminalView() {
    m_alive.reset();
    ghostty_key_event_free(m_keyEvent);
    ghostty_key_encoder_free(m_encoder);
    ghostty_render_state_row_cells_free(m_cells);
    ghostty_render_state_row_iterator_free(m_rows);
    ghostty_render_state_free(m_state);
  }

  bool TerminalView::start(std::vector<std::string> argv, std::string cwd) {
    std::vector<std::string> env = {"TERM=xterm-256color", "COLORTERM=truecolor", "TERM_PROGRAM=andromeda-terminal"};
    if (!m_session.spawn({.argv = std::move(argv), .cwd = std::move(cwd), .env = std::move(env)})) {
      return false;
    }
    applyPalette();
    return true;
  }

  void TerminalView::applyPalette() {
    m_dark = [] {
      const Color& label = colorForRole(ColorRole::Label1);
      return 0.2126F * label.r + 0.7152F * label.g + 0.0722F * label.b > 0.5F;
    }();
    m_background = m_dark ? kViewDark : kViewLight;
    m_foreground = over(colorForRole(ColorRole::Label1), m_background);
    m_faint = over(colorForRole(ColorRole::Label2), m_background);
    m_selection = mix(m_background, over(colorForRole(ColorRole::Accent), m_background), 0.4F);
    const auto& ansi = m_dark ? kAnsiDark : kAnsiLight;
    m_session.withTerminal([&](GhosttyTerminal t) {
      if (t == nullptr) {
        return;
      }
      GhosttyColorRgb palette[256]{};
      ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_COLOR_PALETTE_DEFAULT, palette);
      for (int i = 0; i < 6; ++i) {
        palette[1 + i] = unpack(ansi[i]);
        palette[9 + i] = unpack(ansi[i]);
      }
      ghostty_terminal_set(t, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, palette);
      const GhosttyColorRgb fg = unpack(m_foreground);
      const GhosttyColorRgb bg = unpack(m_background);
      ghostty_terminal_set(t, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &fg);
      ghostty_terminal_set(t, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &bg);
    });
  }

  std::unique_ptr<Node> TerminalView::build(Renderer& renderer, float uiScale, float deviceScale, float width,
                                            float height) {
    m_uiScale = uiScale;
    m_textures = &renderer.textureManager();
    auto grid = std::make_unique<TerminalGridNode>();
    m_grid = grid.get();
    m_grid->setFont(kFontFamily, kFontSize * uiScale, kLineHeight * uiScale, deviceScale);
    m_grid->setGridOrigin(kInset * uiScale, kTopInset * uiScale);
    auto area = ui::inputArea({.out = &m_area,
                               .focusable = true,
                               .width = width,
                               .height = height,
                               .configure =
                                   [this](InputArea& a) {
                                     a.setCapturesAllKeys(true);
                                     a.setRetainsFocusOnPointerRelease(true);
                                     a.setOnFocusGain([this] {
                                       m_areaFocused = true;
                                       updateFocus();
                                     });
                                     a.setOnFocusLoss([this] {
                                       m_areaFocused = false;
                                       updateFocus();
                                     });
                                     a.setOnPress([this](const InputArea::PointerData& d) {
                                       if (d.button == BTN_LEFT) {
                                         pointerPress(d.localX, d.localY, d.pressed);
                                       }
                                     });
                                     a.setOnMotion(
                                         [this](const InputArea::PointerData& d) { pointerMotion(d.localX, d.localY); });
                                   }},
                              std::move(grid));
    setSize(width, height);
    refresh(true);
    return area;
  }

  void TerminalView::setSize(float width, float height) {
    if (m_grid == nullptr || m_grid->cellWidth() <= 0.0F || m_grid->cellHeight() <= 0.0F) {
      return;
    }
    if (m_area != nullptr) {
      m_area->setSize(width, height);
    }
    m_grid->setSize(width, height);
    const float inset = kInset * m_uiScale;
    const int cols = std::max(2, static_cast<int>((width - 2.0F * inset) / m_grid->cellWidth()));
    const int rows = std::max(1, static_cast<int>((height - kTopInset * m_uiScale - inset) / m_grid->cellHeight()));
    if (cols == m_grid->columns() && rows == m_grid->rows()) {
      return;
    }
    m_grid->setGridSize(cols, rows);
    m_session.resize(static_cast<std::uint16_t>(cols), static_cast<std::uint16_t>(rows),
                     static_cast<std::uint16_t>(std::lround(m_grid->cellWidth())),
                     static_cast<std::uint16_t>(std::lround(m_grid->cellHeight())));
    refresh(true);
  }

  void TerminalView::frame() {
    const Redraw redraw = std::exchange(m_redraw, Redraw::None);
    if (!m_session.takeWake()) {
      if (redraw != Redraw::None) {
        refresh(redraw == Redraw::All);
      }
      return;
    }
    if (m_session.state() == SessionState::Exited && !m_exitReported) {
      m_exitReported = true;
      if (m_host.exited) {
        m_host.exited(m_session.exitStatus().value_or(0));
      }
    }
    // Synchronized output (mode 2026): keep the last frame; the release wakes us again.
    if (m_session.renderHeld()) {
      m_redraw = redraw;
      return;
    }
    refresh(redraw == Redraw::All);
  }

  void TerminalView::refresh(bool all) {
    if (m_grid == nullptr) {
      return;
    }
    const bool updated = m_session.withTerminal([&](GhosttyTerminal t) {
      return t != nullptr && ghostty_render_state_begin_update(m_state, t) == GHOSTTY_SUCCESS;
    });
    if (!updated || ghostty_render_state_end_update(m_state) != GHOSTTY_SUCCESS) {
      return;
    }
    GhosttyRenderStateColors colors{};
    colors.size = sizeof(colors);
    ghostty_render_state_get(m_state, GHOSTTY_RENDER_STATE_DATA_COLORS, &colors);
    const std::uint32_t defaultFg = pack(colors.foreground);
    const std::uint32_t defaultBg = pack(colors.background);
    m_grid->setBackground(defaultBg);

    ghostty_render_state_get(m_state, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &m_rows);
    std::uint16_t index = 0;
    std::uint16_t y = 0;
    auto nextRow = [&] {
      if (!all) {
        return ghostty_render_state_row_iterator_next_dirty(m_rows, &y);
      }
      if (!ghostty_render_state_row_iterator_next(m_rows)) {
        return false;
      }
      y = index++;
      return true;
    };
    const bool clean = false;
    while (nextRow()) {
      ghostty_render_state_row_get(m_rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &m_cells);
      auto selected = GHOSTTY_INIT_SIZED(GhosttyRenderStateRowSelection);
      const bool rowSelected =
          ghostty_render_state_row_get(m_rows, GHOSTTY_RENDER_STATE_ROW_DATA_SELECTION, &selected) == GHOSTTY_SUCCESS;
      m_rowBuffer.clear();
      while (ghostty_render_state_row_cells_next(m_cells)) {
        TerminalCell cell{};
        std::uint32_t codepoints[4];
        GhosttyCodepointBuffer graphemes{.ptr = codepoints, .cap = 4, .len = 0};
        ghostty_render_state_row_cells_get(m_cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, &graphemes);
        cell.codepoint = graphemes.len > 0 ? codepoints[0] : 0;

        GhosttyStyle style{};
        ghostty_render_state_row_cells_get(m_cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style);
        GhosttyColorRgb rgb{};
        std::uint32_t fg = defaultFg;
        std::uint32_t bg = defaultBg;
        bool hasBg = false;
        if (ghostty_render_state_row_cells_get(m_cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &rgb)
            == GHOSTTY_SUCCESS) {
          fg = pack(rgb);
        }
        if (ghostty_render_state_row_cells_get(m_cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &rgb)
            == GHOSTTY_SUCCESS) {
          bg = pack(rgb);
          hasBg = true;
        }
        const GhosttyStyleFlags flags = style.flags;
        if ((flags & GHOSTTY_STYLE_FLAG_INVERSE) != 0) {
          std::swap(fg, bg);
          hasBg = true;
        }
        if ((flags & GHOSTTY_STYLE_FLAG_FAINT) != 0) {
          fg = fg == defaultFg ? m_faint : mix(fg, bg, 0.4F);
        }
        if ((flags & GHOSTTY_STYLE_FLAG_INVISIBLE) != 0) {
          cell.codepoint = 0;
        }
        if (rowSelected && m_rowBuffer.size() >= selected.start_x && m_rowBuffer.size() <= selected.end_x) {
          bg = m_selection;
          hasBg = true;
        }
        cell.fg = fg;
        cell.bg = bg;
        cell.flags = static_cast<std::uint16_t>(
            (hasBg ? TerminalCell::Background : 0) | ((flags & GHOSTTY_STYLE_FLAG_BOLD) != 0 ? TerminalCell::Bold : 0)
            | ((flags & GHOSTTY_STYLE_FLAG_ITALIC) != 0 ? TerminalCell::Italic : 0)
            | (GHOSTTY_STYLE_UNDERLINE(style) != 0 ? TerminalCell::Underline : 0)
            | ((flags & GHOSTTY_STYLE_FLAG_STRIKETHROUGH) != 0 ? TerminalCell::Strikethrough : 0)
            | ((flags & GHOSTTY_STYLE_FLAG_OVERLINE) != 0 ? TerminalCell::Overline : 0));

        GhosttyCell raw = 0;
        GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
        if (ghostty_render_state_row_cells_get(m_cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw) == GHOSTTY_SUCCESS
            && ghostty_cell_get(raw, GHOSTTY_CELL_DATA_WIDE, &wide) == GHOSTTY_SUCCESS) {
          if (wide == GHOSTTY_CELL_WIDE_WIDE) {
            cell.flags |= TerminalCell::Wide;
          } else if (wide != GHOSTTY_CELL_WIDE_NARROW) {
            cell.codepoint = 0;
          }
        }
        m_rowBuffer.push_back(cell);
      }
      m_grid->setRow(y, m_rowBuffer);
      ghostty_render_state_row_set(m_rows, GHOSTTY_RENDER_STATE_ROW_OPT_DIRTY, &clean);
    }
    const GhosttyRenderStateDirty none = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
    ghostty_render_state_set(m_state, GHOSTTY_RENDER_STATE_OPT_DIRTY, &none);

    bool visible = false;
    bool inViewport = false;
    ghostty_render_state_get(m_state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE, &visible);
    ghostty_render_state_get(m_state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE, &inViewport);
    if (visible && inViewport) {
      std::uint16_t cx = 0;
      std::uint16_t cy = 0;
      bool wideTail = false;
      GhosttyCursorStyle visual = GHOSTTY_CURSOR_STYLE_BLOCK;
      ghostty_render_state_get(m_state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X, &cx);
      ghostty_render_state_get(m_state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &cy);
      ghostty_render_state_get(m_state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_WIDE_TAIL, &wideTail);
      ghostty_render_state_get(m_state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISUAL_STYLE, &visual);
      TerminalCursorStyle shape = TerminalCursorStyle::Block;
      switch (visual) {
      case GHOSTTY_CURSOR_STYLE_BAR: shape = TerminalCursorStyle::Bar; break;
      case GHOSTTY_CURSOR_STYLE_UNDERLINE: shape = TerminalCursorStyle::Underline; break;
      case GHOSTTY_CURSOR_STYLE_BLOCK_HOLLOW: shape = TerminalCursorStyle::HollowBlock; break;
      default: break;
      }
      m_grid->setCursor(TerminalCursor{
          .column = wideTail && cx > 0 ? cx - 1 : cx,
          .row = cy,
          .style = m_focused ? shape : TerminalCursorStyle::HollowBlock,
          .color = colors.cursor_has_value ? pack(colors.cursor) : defaultFg,
          .textColor = defaultBg,
      });
    } else {
      m_grid->setCursor(std::nullopt);
    }
    if (m_textures != nullptr) {
      m_grid->commit(*m_textures);
    }
  }

  bool TerminalView::key(const KeyboardEvent& event) {
    if (event.preedit) {
      return false;
    }
    if (!event.pressed) {
      if (m_keysDown.erase(event.key) == 0) {
        return false;
      }
    } else {
      const std::uint32_t mods = event.modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Shift | KeyMod::Super);
      const bool windowChord = (mods & KeyMod::Super) != 0
                               || ((mods & (KeyMod::Ctrl | KeyMod::Shift)) == (KeyMod::Ctrl | KeyMod::Shift)
                                   && event.unshifted >= 0x20 && event.unshifted < 0x7f);
      if (windowChord) {
        if (mods == (KeyMod::Ctrl | KeyMod::Shift) && (event.unshifted == 'c' || event.unshifted == 'v')) {
          event.unshifted == 'c' ? copySelection() : paste();
          return true;
        }
        return false;
      }
      m_keysDown.insert(event.key);
    }

    GhosttyMods mods = 0;
    mods |= (event.modifiers & KeyMod::Shift) != 0 ? GHOSTTY_MODS_SHIFT : 0;
    mods |= (event.modifiers & KeyMod::Ctrl) != 0 ? GHOSTTY_MODS_CTRL : 0;
    mods |= (event.modifiers & KeyMod::Alt) != 0 ? GHOSTTY_MODS_ALT : 0;
    // Text excludes C0 controls and DEL: the encoder builds those from the key and mods.
    const bool text = event.utf32 >= 0x20 && event.utf32 != 0x7f && !(event.utf32 >= 0xE000 && event.utf32 < 0xF900);
    std::string utf8;
    if (text) {
      appendUtf8(utf8, event.utf32);
    }
    ghostty_key_event_set_action(m_keyEvent, !event.pressed ? GHOSTTY_KEY_ACTION_RELEASE
                                             : event.repeat ? GHOSTTY_KEY_ACTION_REPEAT
                                                            : GHOSTTY_KEY_ACTION_PRESS);
    ghostty_key_event_set_key(m_keyEvent, ghosttyKey(event.key, event.sym, event.unshifted));
    ghostty_key_event_set_mods(m_keyEvent, mods);
    // xkb used Shift to pick the text ("A", "!"), so the encoder must not add it again.
    ghostty_key_event_set_consumed_mods(m_keyEvent, text ? static_cast<GhosttyMods>(mods & GHOSTTY_MODS_SHIFT) : 0);
    ghostty_key_event_set_utf8(m_keyEvent, {reinterpret_cast<const std::uint8_t*>(utf8.data()), utf8.size()});
    ghostty_key_event_set_unshifted_codepoint(m_keyEvent, event.unshifted);

    m_session.withTerminal([&](GhosttyTerminal t) {
      if (t != nullptr) {
        ghostty_key_encoder_set_from_terminal(m_encoder, t);
      }
    });
    const bool altPrefix = true;
    ghostty_key_encoder_set(m_encoder, GHOSTTY_KEY_ENCODER_OPT_ALT_ESC_PREFIX, &altPrefix);
    std::uint8_t buf[128];
    std::size_t written = 0;
    if (ghostty_key_encoder_encode(m_encoder, m_keyEvent, buf, sizeof(buf), &written) == GHOSTTY_SUCCESS && written > 0) {
      m_session.write({buf, written});
    }
    return true;
  }

  void TerminalView::setWindowActive(bool active) {
    m_windowActive = active;
    updateFocus();
  }

  void TerminalView::updateFocus() {
    const bool focused = m_windowActive && m_areaFocused;
    if (focused == m_focused) {
      return;
    }
    m_focused = focused;
    if (!focused) {
      m_keysDown.clear();
    }
    const bool report = m_session.withTerminal([](GhosttyTerminal t) {
      GhosttyTerminalModeConfig mode{.mode = GHOSTTY_MODE_FOCUS_EVENT, .value = false};
      return t != nullptr && ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_MODE, &mode) == GHOSTTY_SUCCESS && mode.value;
    });
    if (report) {
      std::uint8_t buf[8];
      std::size_t written = 0;
      if (ghostty_focus_encode(focused ? GHOSTTY_FOCUS_GAINED : GHOSTTY_FOCUS_LOST, buf, sizeof(buf), &written)
          == GHOSTTY_SUCCESS) {
        m_session.write({buf, written});
      }
    }
    // Runs during scene teardown too: the grid may be gone, so the next frame redraws the cursor.
    scheduleRedraw(Redraw::Dirty);
  }

  void TerminalView::scheduleRedraw(Redraw redraw) {
    m_redraw = std::max(m_redraw, redraw);
    if (m_host.requestFrame) {
      m_host.requestFrame();
    }
  }

  GhosttyPointCoordinate TerminalView::cellAt(float localX, float localY) const {
    const float col = std::floor((localX - kInset * m_uiScale) / m_grid->cellWidth());
    const float row = std::floor((localY - kTopInset * m_uiScale) / m_grid->cellHeight());
    return {.x = static_cast<std::uint16_t>(std::clamp(col, 0.0F, static_cast<float>(m_grid->columns() - 1))),
            .y = static_cast<std::uint32_t>(std::clamp(row, 0.0F, static_cast<float>(m_grid->rows() - 1)))};
  }

  void TerminalView::pointerPress(float localX, float localY, bool pressed) {
    if (m_grid == nullptr || m_grid->columns() <= 0 || m_grid->rows() <= 0) {
      return;
    }
    if (pressed) {
      m_drag = DragSelecting{.anchor = cellAt(localX, localY)};
      return;
    }
    const auto* drag = std::get_if<DragSelecting>(&m_drag);
    // A click without a drag clears the selection.
    if (drag != nullptr && !drag->moved) {
      m_session.withTerminal([](GhosttyTerminal t) {
        if (t != nullptr) {
          ghostty_terminal_set(t, GHOSTTY_TERMINAL_OPT_SELECTION, nullptr);
        }
      });
      scheduleRedraw(Redraw::All);
    }
    m_drag = DragIdle{};
  }

  void TerminalView::pointerMotion(float localX, float localY) {
    auto* drag = std::get_if<DragSelecting>(&m_drag);
    if (drag == nullptr || m_grid == nullptr) {
      return;
    }
    const GhosttyPointCoordinate cell = cellAt(localX, localY);
    if (!drag->moved && cell.x == drag->anchor.x && cell.y == drag->anchor.y) {
      return;
    }
    drag->moved = true;
    const bool set = m_session.withTerminal([&](GhosttyTerminal t) {
      GhosttySelection selection{};
      return t != nullptr
             && ghostty_terminal_grid_ref(t, {.tag = GHOSTTY_POINT_TAG_VIEWPORT, .value = {.coordinate = drag->anchor}},
                                          &selection.start)
                    == GHOSTTY_SUCCESS
             && ghostty_terminal_grid_ref(t, {.tag = GHOSTTY_POINT_TAG_VIEWPORT, .value = {.coordinate = cell}},
                                          &selection.end)
                    == GHOSTTY_SUCCESS
             && ghostty_terminal_set(t, GHOSTTY_TERMINAL_OPT_SELECTION, &selection) == GHOSTTY_SUCCESS;
    });
    if (set) {
      scheduleRedraw(Redraw::All);
    }
  }

  void TerminalView::copySelection() {
    std::string text;
    m_session.withTerminal([&](GhosttyTerminal t) {
      GhosttySelection selection{};
      GhosttyFormatter formatter = nullptr;
      if (t == nullptr || ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_SELECTION, &selection) != GHOSTTY_SUCCESS
          || ghostty_formatter_terminal_new(nullptr, &formatter, t) != GHOSTTY_SUCCESS) {
        return;
      }
      const bool on = true;
      ghostty_formatter_set(formatter, GHOSTTY_FORMATTER_OPT_SELECTION, &selection);
      ghostty_formatter_set(formatter, GHOSTTY_FORMATTER_OPT_UNWRAP, &on);
      ghostty_formatter_set(formatter, GHOSTTY_FORMATTER_OPT_TRIM, &on);
      std::uint8_t* ptr = nullptr;
      std::size_t len = 0;
      if (ghostty_formatter_format_alloc(formatter, nullptr, &ptr, &len) == GHOSTTY_SUCCESS) {
        text.assign(reinterpret_cast<const char*>(ptr), len);
        ghostty_free(nullptr, ptr, len);
      }
      ghostty_formatter_free(formatter);
    });
    TextClipboard* clipboard = Input::textClipboard();
    if (!text.empty() && clipboard != nullptr) {
      clipboard->setClipboardText(std::move(text));
    }
  }

  void TerminalView::paste() {
    TextClipboard* clipboard = Input::textClipboard();
    std::optional<std::string> text = clipboard != nullptr ? clipboard->clipboardText() : std::nullopt;
    if (!text || text->empty()) {
      return;
    }
    const bool bracketed = m_session.withTerminal([](GhosttyTerminal t) {
      GhosttyTerminalModeConfig mode{.mode = GHOSTTY_MODE_BRACKETED_PASTE, .value = false};
      return t != nullptr && ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_MODE, &mode) == GHOSTTY_SUCCESS && mode.value;
    });
    std::size_t needed = 0;
    ghostty_paste_encode(text->data(), text->size(), bracketed, nullptr, 0, &needed);
    std::vector<std::uint8_t> buf(needed);
    std::size_t written = 0;
    if (ghostty_paste_encode(text->data(), text->size(), bracketed, buf.data(), buf.size(), &written)
            == GHOSTTY_SUCCESS
        && written > 0) {
      m_session.write({buf.data(), written});
    }
  }

} // namespace term
