#include "terminal_view.h"

#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/palette.h"

#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>
#include <string>

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
  }

  TerminalView::~TerminalView() {
    m_alive.reset();
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
                                     a.setOnKeyDown([this](const InputArea::KeyData& k) {
                                       key(k.sym, k.utf32, k.modifiers);
                                     });
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
    if (!m_session.takeWake()) {
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
      return;
    }
    refresh(false);
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
          .style = shape,
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

  // Minimal xterm encoding until step 4 moves keys to libghostty's key encoder.
  void TerminalView::key(std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers) {
    std::string out;
    const bool ctrl = (modifiers & KeyMod::Ctrl) != 0;
    const bool alt = (modifiers & KeyMod::Alt) != 0;
    switch (sym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter: out = "\r"; break;
    case XKB_KEY_BackSpace: out = ctrl ? "\x08" : "\x7f"; break;
    case XKB_KEY_Tab: out = "\t"; break;
    case XKB_KEY_ISO_Left_Tab: out = "\x1b[Z"; break;
    case XKB_KEY_Escape: out = "\x1b"; break;
    case XKB_KEY_Up: out = "\x1b[A"; break;
    case XKB_KEY_Down: out = "\x1b[B"; break;
    case XKB_KEY_Right: out = "\x1b[C"; break;
    case XKB_KEY_Left: out = "\x1b[D"; break;
    case XKB_KEY_Home: out = "\x1b[H"; break;
    case XKB_KEY_End: out = "\x1b[F"; break;
    case XKB_KEY_Insert: out = "\x1b[2~"; break;
    case XKB_KEY_Delete: out = "\x1b[3~"; break;
    case XKB_KEY_Page_Up: out = "\x1b[5~"; break;
    case XKB_KEY_Page_Down: out = "\x1b[6~"; break;
    default:
      if (ctrl && utf32 != 0 && utf32 < 0x80) {
        const auto c = static_cast<char>(utf32 >= 'a' && utf32 <= 'z' ? utf32 - 'a' + 'A' : utf32);
        if (c >= '@' && c <= '_') {
          out += static_cast<char>(c - '@');
        } else if (c == ' ') {
          out += '\0';
        }
      } else if (utf32 != 0) {
        appendUtf8(out, utf32);
      }
    }
    if (out.empty()) {
      return;
    }
    if (alt) {
      out.insert(out.begin(), '\x1b');
    }
    m_session.write({reinterpret_cast<const std::uint8_t*>(out.data()), out.size()});
  }

} // namespace term
