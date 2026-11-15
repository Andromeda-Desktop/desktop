#include "terminal_view.h"

#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "git_branch.h"
#include "key_map.h"
#include "kitty_images.h"
#include "link_finder.h"
#include "wayland/wayland_seat.h"
#include "render/animation/motion_service.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "core/text_clipboard.h"
#include "ui/builders.h"
#include "ui/controls/input.h"
#include "ui/controls/scrollbar.h"
#include "ui/palette.h"
#include "time/time_format.h"
#include "ui/style.h"
#include "wayland/text_input_service.h"

#include "cursor-shape-v1-client-protocol.h"

#include <unistd.h>
#include <linux/input-event-codes.h>

#include <wayland-client-protocol.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <cstdlib>
#include <optional>
#include <thread>
#include <string>
#include <string_view>
#include <utility>

namespace term {

  namespace {

    const float& kInset = Tokens::spaceP24;
    const float& kTopInset = Tokens::spaceP8;

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

    std::vector<char32_t> decodeUtf8(std::string_view s) {
      std::vector<char32_t> out;
      for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        const int len = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 1;
        char32_t cp = len == 1 ? b : b & (0x7F >> len);
        for (int k = 1; k < len && i + k < s.size(); ++k) {
          cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        out.push_back(cp);
        i += static_cast<std::size_t>(len);
      }
      return out;
    }

    GhosttyMods ghosttyMods(std::uint32_t mods) {
      GhosttyMods out = 0;
      out |= (mods & KeyMod::Shift) != 0 ? GHOSTTY_MODS_SHIFT : 0;
      out |= (mods & KeyMod::Ctrl) != 0 ? GHOSTTY_MODS_CTRL : 0;
      out |= (mods & KeyMod::Alt) != 0 ? GHOSTTY_MODS_ALT : 0;
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
        }, [this, alive = std::weak_ptr<bool>(m_alive)](const CommandMark& mark) {
          // IO thread, under the terminal lock.
          DeferredCall::callLater([this, alive, mark] {
            if (!alive.expired()) {
              onMark(mark);
            }
          });
        }) {
    ghostty_render_state_new(nullptr, &m_state);
    ghostty_render_state_row_iterator_new(nullptr, &m_rows);
    ghostty_render_state_row_cells_new(nullptr, &m_cells);
    // One row below the viewport: a pixel scroll shows part of it.
    const GhosttyRenderStateOverscan overscan{.above = 0, .below = 1};
    ghostty_render_state_set(m_state, GHOSTTY_RENDER_STATE_OPT_OVERSCAN, &overscan);
    ghostty_key_encoder_new(nullptr, &m_encoder);
    ghostty_key_event_new(nullptr, &m_keyEvent);
    ghostty_mouse_encoder_new(nullptr, &m_mouseEncoder);
    ghostty_mouse_event_new(nullptr, &m_mouseEvent);
  }

  TerminalView::~TerminalView() {
    m_alive.reset();
    if (std::holds_alternative<TerminalFind>(m_find)) {
      m_session.withTerminal([&](GhosttyTerminal) { m_find = FindClosed{}; });
    }
    if (m_textInput != nullptr) {
      m_textInput->clearFocusedClient(this);
    }
    ghostty_mouse_event_free(m_mouseEvent);
    ghostty_mouse_encoder_free(m_mouseEncoder);
    ghostty_key_event_free(m_keyEvent);
    ghostty_key_encoder_free(m_encoder);
    ghostty_render_state_row_cells_free(m_cells);
    ghostty_render_state_row_iterator_free(m_rows);
    ghostty_render_state_free(m_state);
  }

  bool TerminalView::start(std::vector<std::string> argv, std::string cwd, const std::vector<std::string>& extraEnv) {
    std::vector<std::string> env = {"TERM=xterm-256color", "COLORTERM=truecolor", "TERM_PROGRAM=andromeda-terminal"};
    env.insert(env.end(), extraEnv.begin(), extraEnv.end());
    if (!m_session.spawn({.argv = std::move(argv), .cwd = std::move(cwd), .env = std::move(env)})) {
      return false;
    }
    applyPalette();
    return true;
  }

  bool TerminalView::reattach(std::uint64_t heldId) {
    if (!m_session.attach(heldId, 80, 24, 8, 16)) {
      return false;
    }
    applyPalette();
    return true;
  }

  void TerminalView::applyPalette() {
    m_background = over(colorForRole(ColorRole::View), 0);
    m_foreground = over(colorForRole(ColorRole::Label1), m_background);
    m_faint = over(colorForRole(ColorRole::Label2), m_background);
    m_selection = mix(m_background, over(colorForRole(ColorRole::Accent), m_background), 0.4F);
    m_findHit = colorForRole(ColorRole::TermFindHit);
    m_findHitCurrent = colorForRole(ColorRole::TermFindHitCurrent);
    // ANSI 1-6; the bright set (9-14) repeats them.
    constexpr std::array<ColorRole, 6> kAnsi = {ColorRole::TermRed,  ColorRole::TermGreen,   ColorRole::TermYellow,
                                                ColorRole::TermBlue, ColorRole::TermMagenta, ColorRole::TermCyan};
    m_session.withTerminal([&](GhosttyTerminal t) {
      if (t == nullptr) {
        return;
      }
      GhosttyColorRgb palette[256]{};
      ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_COLOR_PALETTE_DEFAULT, palette);
      for (int i = 0; i < 6; ++i) {
        palette[1 + i] = unpack(over(colorForRole(kAnsi[i]), m_background));
        palette[9 + i] = palette[1 + i];
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
    m_deviceScale = deviceScale;
    m_pendingDeviceScale = deviceScale;
    // A rebuild follows every palette change (Appearance::apply), so the grid colours track the theme.
    applyPalette();
    auto grid = std::make_unique<TerminalGridNode>();
    m_grid = grid.get();
    m_grid->setFont(std::string(Tokens::fontMono), Tokens::typeMono.size * uiScale, Tokens::typeMono.line * uiScale, deviceScale);
    m_images = std::make_unique<KittyImages>(*m_grid);
    m_context = std::make_unique<ContextRow>(*m_grid);
    auto findLayer = std::make_unique<Node>();
    findLayer->setZIndex(TerminalGridNode::kBelowTextZ);
    m_findRing = static_cast<RectNode*>(findLayer->addChild(std::make_unique<RectNode>()));
    m_findRing->setVisible(false);
    m_findLayer = m_grid->addChild(std::move(findLayer));
    setGridOrigin(kInset * uiScale, kTopInset * uiScale);
    auto area = ui::inputArea({.out = &m_area,
                               .focusable = true,
                               .width = width,
                               .height = height,
                               .flexGrow = 1.0F,
                               .configure =
                                   [this](InputArea& a) {
                                     a.setCapturesAllKeys(true);
                                     a.setCapturesTab(true);
                                     a.setRetainsFocusOnPointerRelease(true);
                                     a.setTextInputClient(this);
                                     a.setOnFocusGain([this] {
                                       m_areaFocused = true;
                                       updateFocus();
                                     });
                                     a.setOnFocusLoss([this] {
                                       m_areaFocused = false;
                                       updateFocus();
                                       updateLink(false); // Ctrl may come up in another window
                                     });
                                     a.setOnLeave([this] { updateLink(false); });
                                     a.setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_MIDDLE, BTN_RIGHT}));
                                     a.setOnPress([this](const InputArea::PointerData& d) {
                                       pointerButton(d.button, d.localX, d.localY, d.pressed, d.time);
                                     });
                                     a.setOnMotion(
                                         [this](const InputArea::PointerData& d) { pointerMotion(d.localX, d.localY); });
                                     a.setOnAxisHandler([this](const InputArea::PointerData& d) { return wheel(d); });
                                     a.setOnAxisStop([this](std::uint32_t axis) {
                                       if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL
                                           && m_scrollMotion == ScrollMotion::Finger) {
                                         settleScroll();
                                       }
                                     });
                                   }},
                              std::move(grid));
    // The partial rows of a pixel scroll stay inside the view.
    area->setClipChildren(true);
    auto bar = std::make_unique<Scrollbar>();
    m_scrollbar = bar.get();
    m_scrollState = {};
    m_scrollbar->setAutoHide(true);
    m_scrollbar->setContentScale(uiScale);
    m_scrollbar->setOnScrollChanged([this](float offset) {
      const float cellH = m_grid->cellHeight();
      animateScroll(std::round(offset / cellH) * cellH, Tokens::springFollow, 0.0F, true);
    });
    area->addChild(std::move(bar));
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
    if (m_scrollbar != nullptr) {
      const float thickness = m_scrollbar->reservedThickness();
      m_scrollbar->setPosition(width - thickness, 0.0F);
      m_scrollbar->setSize(thickness, height);
      const float cellH = m_grid->cellHeight();
      m_scrollbar->update(static_cast<float>(m_scrollState.len) * cellH, static_cast<float>(m_scrollState.total) * cellH,
                          m_scrollPx);
    }
    m_width = width;
    m_height = height;
    const float inset = kInset * m_uiScale;
    const int cols = std::max(2, static_cast<int>((width - 2.0F * inset) / m_grid->cellWidth()));
    const int rows = std::max(1, static_cast<int>((height - kTopInset * m_uiScale - inset) / m_grid->cellHeight()));
    // Device px: images a program sizes from the cell size stay sharp at any scale.
    const std::array<std::uint16_t, 2> cellPx{static_cast<std::uint16_t>(std::lround(m_grid->cellWidth() * m_deviceScale)),
                                              static_cast<std::uint16_t>(std::lround(m_grid->cellHeight() * m_deviceScale))};
    if (cols == m_grid->columns() && rows == viewRows() && cellPx == m_cellPx) {
      return;
    }
    m_cellPx = cellPx;
    m_grid->setGridSize(cols, rows + 1);
    m_session.resize(static_cast<std::uint16_t>(cols), static_cast<std::uint16_t>(rows), cellPx[0], cellPx[1]);
    refresh(true);
  }

  void TerminalView::setDeviceScale(float deviceScale) {
    m_pendingDeviceScale = deviceScale;
    scheduleRedraw(Redraw::All);
  }

  void TerminalView::frame() {
    if (m_grid != nullptr) {
      commitAtlas();
    }
    // Output or a scroll can move the text under a shown link.
    if (m_link) {
      updateLink(true);
    }
    if (m_grid != nullptr && m_pendingDeviceScale != m_deviceScale) {
      m_deviceScale = m_pendingDeviceScale;
      m_grid->setFont(std::string(Tokens::fontMono), Tokens::typeMono.size * m_uiScale, Tokens::typeMono.line * m_uiScale, m_deviceScale);
      setSize(m_width, m_height);
    }
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
    GhosttyTerminalScrollbar scroll{};
    auto* find = std::get_if<TerminalFind>(&m_find);
    const auto findHits = find != nullptr ? find->hits() : std::vector<FindHit>{};
    const auto findCurrent = find != nullptr ? find->current() : std::nullopt;
    bool findChanged = false;
    const bool updated = m_session.withTerminal([&](GhosttyTerminal t) {
      if (t == nullptr) {
        return false;
      }
      // Before the render state: selecting the first match can scroll the viewport.
      if (find != nullptr) {
        findChanged = find->read(t);
      }
      if (ghostty_render_state_begin_update(m_state, t) != GHOSTTY_SUCCESS) {
        return false;
      }
      ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &scroll);
      return true;
    });
    if (!updated || ghostty_render_state_end_update(m_state) != GHOSTTY_SUCCESS) {
      return;
    }
    if (find != nullptr) {
      // Bounded progress off the lock; the next frame feeds the search again until it completes.
      if (find->tick(std::chrono::milliseconds(2))) {
        scheduleRedraw(Redraw::All);
      }
      if (findChanged) {
        m_marksDirty = true;
        if (m_host.findChanged) {
          m_host.findChanged(find->total(), find->index() ? std::optional<std::size_t>(*find->index() + 1) : std::nullopt);
        }
      }
      if (find->hits() != findHits || find->current() != findCurrent) {
        all = true;
      }
      const auto& cur = find->current();
      m_findRing->setVisible(cur.has_value() && cur->start.y == cur->end.y);
      if (cur && cur->start.y == cur->end.y) {
        const float cw = m_grid->cellWidth();
        const float ch = m_grid->cellHeight();
        const float ring = Tokens::componentTerminalFindRing * m_uiScale;
        m_findRing->setStyle(RoundedRectStyle{.fill = Color{0.0F, 0.0F, 0.0F, 0.0F},
                                              .border = colorForRole(ColorRole::TermFindRing),
                                              .radius = Radii(Tokens::componentTerminalFindHitRadius * m_uiScale + ring),
                                              .borderWidth = ring});
        m_findRing->setPosition(static_cast<float>(cur->start.x) * cw - ring, static_cast<float>(cur->start.y) * ch - ring);
        m_findRing->setFrameSize(static_cast<float>(cur->end.x - cur->start.x + 1) * cw + 2.0F * ring, ch + 2.0F * ring);
      }
    } else if (m_findRing != nullptr) {
      m_findRing->setVisible(false);
    }
    if (!m_images->sync(m_state, m_session, m_host.textures ? m_host.textures() : nullptr, m_grid->cellWidth(),
                        m_grid->cellHeight(), m_deviceScale)) {
      scheduleRedraw(Redraw::Dirty);
    }
    if (scroll.offset != m_viewRow) {
      // Ghostty moved the viewport (output at the bottom, a jump to the bottom, pruned history): back to a whole row.
      if (m_scrollAnim != 0 && m_area != nullptr && m_area->animationManager() != nullptr) {
        m_area->animationManager()->cancel(m_scrollAnim);
      }
      m_scrollAnim = 0;
      m_scrollMotion = ScrollMotion::Rest;
      m_scrollSamples.clear();
      m_viewRow = scroll.offset;
      m_scrollPx = static_cast<float>(scroll.offset) * m_grid->cellHeight();
      setGridOrigin(kInset * m_uiScale, kTopInset * m_uiScale);
    }
    if (m_scrollbar != nullptr
        && (scroll.total != m_scrollState.total || scroll.offset != m_scrollState.offset
            || scroll.len != m_scrollState.len)) {
      m_scrollState = scroll;
      const float cellH = m_grid->cellHeight();
      m_scrollbar->update(static_cast<float>(scroll.len) * cellH, static_cast<float>(scroll.total) * cellH, m_scrollPx);
      m_marksDirty = true;
    }
    if (find == nullptr && m_scrollbar != nullptr) {
      // A full scrollback moves every prompt up a row while total, offset and len stay the same.
      const auto key = m_session.withCommands([](GhosttyTerminal, const CommandIndex& index) {
        const auto& e = index.entries();
        return CommandMarksKey{e.size(), e.empty() ? std::nullopt : CommandIndex::row(e.front().prompt.get(), GHOSTTY_POINT_TAG_SCREEN)};
      });
      m_marksDirty = m_marksDirty || key != std::exchange(m_commandMarksKey, key);
    }
    if (std::exchange(m_marksDirty, false)) {
      updateMarks();
    }
    syncContext();
    GhosttyRenderStateColors colors{};
    colors.size = sizeof(colors);
    ghostty_render_state_get(m_state, GHOSTTY_RENDER_STATE_DATA_COLORS, &colors);
    const std::uint32_t defaultFg = pack(colors.foreground);
    const std::uint32_t defaultBg = pack(colors.background);
    m_grid->setBackground(defaultBg);

    bool visible = false;
    bool inViewport = false;
    std::uint16_t cx = 0;
    std::uint16_t cy = 0;
    ghostty_render_state_get(m_state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE, &visible);
    ghostty_render_state_get(m_state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE, &inViewport);
    if (inViewport) {
      ghostty_render_state_get(m_state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X, &cx);
      ghostty_render_state_get(m_state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &cy);
    }
    // The preedit row must be redrawn when the cursor moves or the preedit changes.
    all = all || !m_preedit.empty();
    int preeditWidth = 0;

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
        if (bool current = false; find != nullptr && find->hitAt(y, static_cast<std::uint16_t>(m_rowBuffer.size()), current)) {
          bg = over(current ? m_findHitCurrent : m_findHit, bg);
          hasBg = true;
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
      if (m_link && m_link->row == y) {
        for (int x = m_link->start; x < std::min<int>(m_link->end, static_cast<int>(m_rowBuffer.size())); ++x) {
          m_rowBuffer[static_cast<std::size_t>(x)].flags |= TerminalCell::Underline;
        }
      }
      if (inViewport && y == cy && !m_preedit.empty()) {
        preeditWidth = overlayPreedit(cx, defaultFg, defaultBg);
      }
      m_grid->setRow(y, m_rowBuffer);
      ghostty_render_state_row_set(m_rows, GHOSTTY_RENDER_STATE_ROW_OPT_DIRTY, &clean);
    }
    const GhosttyRenderStateDirty none = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
    ghostty_render_state_set(m_state, GHOSTTY_RENDER_STATE_OPT_DIRTY, &none);
    GhosttyRenderStateOverscan captured{};
    ghostty_render_state_get(m_state, GHOSTTY_RENDER_STATE_DATA_OVERSCAN, &captured);
    if (captured.below == 0) {
      m_grid->setRow(viewRows(), {});
    }

    if (inViewport && (cx != m_cursorCell.x || cy != m_cursorCell.y)) {
      m_cursorCell = {.x = cx, .y = cy};
      if (m_textInput != nullptr) {
        m_textInput->notifyClientStateChanged(this, TextInputChangeCause::Other);
      }
    }
    if (visible && inViewport) {
      bool wideTail = false;
      GhosttyCursorStyle visual = GHOSTTY_CURSOR_STYLE_BLOCK;
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
          .column = std::min((wideTail && cx > 0 ? cx - 1 : cx) + preeditWidth, m_grid->columns() - 1),
          .row = cy,
          .style = m_focused ? shape : TerminalCursorStyle::HollowBlock,
          .color = colors.cursor_has_value ? pack(colors.cursor) : defaultFg,
          .textColor = defaultBg,
      });
    } else {
      m_grid->setCursor(std::nullopt);
    }
    commitAtlas();
  }

  void TerminalView::commitAtlas() {
    if (TextureManager* textures = m_host.textures ? m_host.textures() : nullptr) {
      m_grid->commit(*textures);
    } else if (m_host.requestFrame) {
      m_host.requestFrame(); // the frame tick runs after GL attaches and before the frame renders
    }
  }

  bool TerminalView::key(const KeyboardEvent& event) {
    if (event.preedit) {
      return false;
    }
    if (event.key == KEY_LEFTCTRL || event.key == KEY_RIGHTCTRL) {
      updateLink(event.pressed);
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

    const GhosttyMods mods = ghosttyMods(event.modifiers);
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
      scrollToBottom();
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

  std::string pathFromFileUri(std::string_view uri) {
    std::string_view rest = uri;
    if (rest.starts_with("file://")) {
      rest.remove_prefix(7);
      const auto slash = rest.find('/');
      const std::string_view host = rest.substr(0, slash);
      std::array<char, 256> own{};
      const bool local = host.empty() || host == "localhost"
          || (gethostname(own.data(), own.size() - 1) == 0 && host == std::string_view(own.data()));
      // A remote shell's cwd is not a local path: no Files, no branch.
      if (!local || slash == std::string_view::npos) {
        return {};
      }
      rest = rest.substr(slash);
    }
    std::string path;
    for (std::size_t i = 0; i < rest.size(); ++i) {
      if (rest[i] == '%' && i + 2 < rest.size() && std::isxdigit(static_cast<unsigned char>(rest[i + 1]))
          && std::isxdigit(static_cast<unsigned char>(rest[i + 2]))) {
        path += static_cast<char>(std::stoi(std::string(rest.substr(i + 1, 2)), nullptr, 16));
        i += 2;
      } else {
        path += rest[i];
      }
    }
    return path;
  }

  void TerminalView::readBranch(const void* prompt, std::string cwdUri, std::string path) {
    // .git/HEAD can sit on a slow or hung network mount: read it off the UI thread.
    std::thread([this, alive = std::weak_ptr<bool>(m_alive), prompt, cwdUri = std::move(cwdUri), path = std::move(path)] {
      auto branch = gitBranch(path);
      DeferredCall::callLater([this, alive, prompt, cwdUri, branch = std::move(branch)] {
        if (alive.expired()) {
          return;
        }
        m_session.withCommands([&](GhosttyTerminal, CommandIndex& index) {
          for (auto& entry : index.entries()) {
            if (entry.prompt.get() == prompt && entry.cwdUri == cwdUri && entry.branchState == CommandEntry::Branch::Pending) {
              entry.branchState = branch ? CommandEntry::Branch::Known : CommandEntry::Branch::None;
              entry.branch = branch.value_or("");
            }
          }
        });
        scheduleRedraw(Redraw::All);
      });
    }).detach();
  }

  void TerminalView::onMark(const CommandMark& mark) {
    constexpr auto kNotifyAfter = std::chrono::seconds(10);
    switch (mark.kind) {
    case CommandMark::Kind::OutputStart:
      m_command = CommandRunning{.start = std::chrono::steady_clock::now(), .text = mark.command};
      if (m_host.commandStarted) {
        m_host.commandStarted(mark.command);
      }
      break;
    case CommandMark::Kind::CommandEnd: {
      const auto* running = std::get_if<CommandRunning>(&m_command);
      const auto elapsed = running != nullptr ? std::chrono::steady_clock::now() - running->start
                                              : std::chrono::steady_clock::duration{};
      if (running != nullptr && elapsed >= kNotifyAfter && !m_focused && m_host.commandFinished) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        const std::string took =
            seconds >= 60 ? std::format("{} min {} s", seconds / 60, seconds % 60) : std::format("{} s", seconds);
        const bool failed = mark.exitCode.value_or(0) != 0;
        std::string body = running->text.empty() ? std::string{"A command"} : running->text;
        body += failed ? std::format(" exited with {} after {}", *mark.exitCode, took) : std::format(" took {}", took);
        m_host.commandFinished(failed ? "Command failed" : "Command finished", body);
      }
      if (running != nullptr && m_host.commandEnded) {
        m_host.commandEnded(mark.exitCode);
      }
      m_command = CommandIdle{};
      break;
    }
    case CommandMark::Kind::PromptStart: updateCwd(); break;
    case CommandMark::Kind::InputStart: break;
    }
  }

  void TerminalView::updateCwd() {
    std::string uri = m_session.withTerminal([](GhosttyTerminal t) {
      GhosttyString pwd{};
      if (t == nullptr || ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_PWD, &pwd) != GHOSTTY_SUCCESS || pwd.len == 0) {
        return std::string{};
      }
      return std::string(reinterpret_cast<const char*>(pwd.ptr), pwd.len);
    });
    const std::string path = pathFromFileUri(uri);
    if (path.empty() || path == m_cwd) {
      return;
    }
    m_cwd = path;
    if (m_host.cwdChanged) {
      m_host.cwdChanged(m_cwd);
    }
  }

  // The image layers move with the grid origin: placements are in cells from the top-left viewport cell.
  void TerminalView::setGridOrigin(float x, float y) {
    m_grid->setGridOrigin(x, y);
    m_images->setOrigin(x, y);
    m_context->setOrigin(x, y);
    m_findLayer->setPosition(x, y);
  }

  void TerminalView::syncContext() {
    using Caption = ContextItem::Caption;
    const auto now = std::chrono::steady_clock::now();
    const std::uint64_t top = m_viewRow;
    const std::uint64_t bottom = top + static_cast<std::uint64_t>(std::max(0, viewRows())) + 1;
    const char* homeEnv = std::getenv("HOME");
    const std::string home = homeEnv != nullptr ? homeEnv : "";
    auto placed = m_session.withCommands([&](GhosttyTerminal t, CommandIndex& index) {
      std::vector<ContextRow::Placed> out;
      GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
      std::uint16_t cols = 0;
      // Prompt refs live on the primary screen; a full-screen program draws over it.
      if (t == nullptr || ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &screen) != GHOSTTY_SUCCESS
          || screen != GHOSTTY_TERMINAL_SCREEN_PRIMARY
          || ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_COLS, &cols) != GHOSTTY_SUCCESS) {
        return out;
      }
      const auto blank = [&](std::uint32_t row) {
        for (std::uint16_t x = 0; x < cols; ++x) {
          GhosttyPoint point{};
          point.tag = GHOSTTY_POINT_TAG_SCREEN;
          point.value.coordinate = {.x = x, .y = row};
          GhosttyGridRef ref{};
          GhosttyCell cell = 0;
          bool text = false;
          if (ghostty_terminal_grid_ref(t, point, &ref) == GHOSTTY_SUCCESS
              && ghostty_grid_ref_cell(&ref, &cell) == GHOSTTY_SUCCESS
              && ghostty_cell_get(cell, GHOSTTY_CELL_DATA_HAS_TEXT, &text) == GHOSTTY_SUCCESS && text) {
            return false;
          }
        }
        return true;
      };
      auto& entries = index.entries();
      for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        const auto row = CommandIndex::row(it->prompt.get(), GHOSTTY_POINT_TAG_SCREEN);
        if (!row || *row < top) {
          break;
        }
        // A user prompt (starship, a styled PS1) keeps its own row: the pills go only in our blank row.
        if (*row >= bottom || !blank(*row)) {
          continue;
        }
        const std::string path = pathFromFileUri(it->cwdUri);
        if (it->branchState == CommandEntry::Branch::Unread) {
          it->branchState = path.empty() ? CommandEntry::Branch::None : CommandEntry::Branch::Pending;
          if (!path.empty()) {
            readBranch(it->prompt.get(), it->cwdUri, path);
          }
        }
        ContextItem item;
        item.path = path;
        const bool inHome = !home.empty() && (path == home || path.starts_with(home + "/"));
        item.cwd = inHome ? "~" + path.substr(home.size()) : path;
        item.branch = it->branch;
        const auto clock = [](std::chrono::steady_clock::duration d) {
          const double seconds = std::chrono::duration<double>(d).count();
          return seconds < 60.0 ? std::format("{:.1f} s", seconds)
                                : formatClockTime(static_cast<std::int64_t>(seconds));
        };
        switch (it->phase) {
        case CommandEntry::Phase::Prompt: break;
        case CommandEntry::Phase::Running:
          item.caption = Caption::Running;
          item.time = "running · " + formatClockTime(std::chrono::duration_cast<std::chrono::seconds>(now - it->started).count());
          break;
        case CommandEntry::Phase::Done:
          item.caption = it->exitCode.value_or(0) == 0 ? Caption::Done : Caption::Failed;
          item.exit = std::format("exit {}", it->exitCode.value_or(0));
          item.time = clock(it->duration);
          break;
        }
        out.push_back({.row = static_cast<std::uint32_t>(*row - top), .item = std::move(item)});
      }
      return out;
    });
    const bool running = std::ranges::any_of(placed, [](const ContextRow::Placed& p) {
      return p.item.caption == Caption::Running;
    });
    const float inset = kInset * m_uiScale;
    m_context->sync(std::move(placed), m_grid->cellHeight(), m_width - 2.0F * inset + Tokens::spaceP8 * m_uiScale,
                    m_uiScale);
    if (running && !m_contextTick.active()) {
      m_contextTick.startRepeating(std::chrono::seconds(1), [this] { syncContext(); });
    } else if (!running) {
      m_contextTick.stop();
    }
  }

  void TerminalView::scheduleRedraw(Redraw redraw) {
    m_redraw = std::max(m_redraw, redraw);
    if (m_host.requestFrame) {
      m_host.requestFrame();
    }
  }

  GhosttyPointCoordinate TerminalView::cellAt(float localX, float localY) const {
    const float col = std::floor((localX - kInset * m_uiScale) / m_grid->cellWidth());
    const float cellH = m_grid->cellHeight();
    const float fraction = m_scrollPx - static_cast<float>(m_viewRow) * cellH;
    const float row = std::floor((localY - kTopInset * m_uiScale + fraction) / cellH);
    return {.x = static_cast<std::uint16_t>(std::clamp(col, 0.0F, static_cast<float>(m_grid->columns() - 1))),
            .y = static_cast<std::uint32_t>(std::clamp(row, 0.0F, static_cast<float>(viewRows() - 1)))};
  }

  int TerminalView::overlayPreedit(int x, std::uint32_t fg, std::uint32_t bg) {
    int col = x;
    for (const char32_t cp : decodeUtf8(m_preedit)) {
      const int width = std::max<int>(1, ghostty_unicode_codepoint_width(cp));
      if (col + width > static_cast<int>(m_rowBuffer.size())) {
        break;
      }
      m_rowBuffer[static_cast<std::size_t>(col)] = TerminalCell{
          .codepoint = cp,
          .fg = fg,
          .bg = bg,
          .flags = static_cast<std::uint16_t>(TerminalCell::Background | TerminalCell::Underline
                                              | (width == 2 ? TerminalCell::Wide : 0)),
      };
      if (width == 2) {
        m_rowBuffer[static_cast<std::size_t>(col) + 1] =
            TerminalCell{.codepoint = 0, .fg = fg, .bg = bg, .flags = TerminalCell::Background};
      }
      col += width;
    }
    return col - x;
  }

  TextInputState TerminalView::textInputState() const {
    float sceneX = 0.0F;
    float sceneY = 0.0F;
    if (m_area != nullptr) {
      Node::absolutePosition(m_area, sceneX, sceneY);
    }
    const float cellW = m_grid != nullptr ? m_grid->cellWidth() : 1.0F;
    const float cellH = m_grid != nullptr ? m_grid->cellHeight() : 1.0F;
    return TextInputState{
        .cursorRectX = static_cast<std::int32_t>(std::lround(sceneX + kInset * m_uiScale + m_cursorCell.x * cellW)),
        .cursorRectY = static_cast<std::int32_t>(
            std::lround(sceneY + kTopInset * m_uiScale + static_cast<float>(m_cursorCell.y) * cellH)),
        .cursorRectWidth = static_cast<std::int32_t>(std::max(1L, std::lround(cellW))),
        .cursorRectHeight = static_cast<std::int32_t>(std::max(1L, std::lround(cellH))),
        .sendSurroundingText = false,
    };
  }

  void TerminalView::textInputApplyEdit(const TextInputEdit& edit) {
    if (edit.hasPreedit && edit.preeditText != m_preedit) {
      m_preedit = edit.preeditText;
      scheduleRedraw(Redraw::All);
    }
    if (edit.hasCommitText && !edit.commitText.empty()) {
      scrollToBottom();
      m_session.write({reinterpret_cast<const std::uint8_t*>(edit.commitText.data()), edit.commitText.size()});
    }
  }

  void TerminalView::textInputResetPreedit() {
    if (!m_preedit.empty()) {
      m_preedit.clear();
      scheduleRedraw(Redraw::All);
    }
  }

  void TerminalView::textInputActivated(TextInputService& service) { m_textInput = &service; }

  void TerminalView::textInputDeactivated(TextInputService& service) {
    if (m_textInput == &service) {
      m_textInput = nullptr;
    }
  }

  std::uint32_t TerminalView::modifiers() const {
    return m_host.modifiers ? m_host.modifiers() : 0;
  }

  std::optional<TerminalView::HoverLink> TerminalView::linkAt(GhosttyPointCoordinate cell) {
    const int columns = m_grid->columns();
    std::vector<char32_t> cells(static_cast<std::size_t>(columns), U' ');
    std::string osc8;
    int start = cell.x;
    int end = cell.x + 1;
    m_session.withTerminal([&](GhosttyTerminal t) {
      if (t == nullptr) {
        return;
      }
      const auto refAt = [&](int x, GhosttyGridRef& ref) {
        const GhosttyPointCoordinate at{.x = static_cast<std::uint16_t>(x), .y = cell.y};
        return ghostty_terminal_grid_ref(t, {.tag = GHOSTTY_POINT_TAG_VIEWPORT, .value = {.coordinate = at}}, &ref)
               == GHOSTTY_SUCCESS;
      };
      const auto uriAt = [&](int x) {
        GhosttyGridRef ref{};
        std::size_t len = 0;
        if (!refAt(x, ref) || ghostty_grid_ref_hyperlink_uri(&ref, nullptr, 0, &len) != GHOSTTY_OUT_OF_SPACE) {
          return std::string();
        }
        std::string uri(len, '\0');
        return ghostty_grid_ref_hyperlink_uri(&ref, reinterpret_cast<std::uint8_t*>(uri.data()), len, &len)
                       == GHOSTTY_SUCCESS
                   ? uri
                   : std::string();
      };
      osc8 = uriAt(cell.x);
      if (!osc8.empty()) {
        while (start > 0 && uriAt(start - 1) == osc8) {
          --start;
        }
        while (end < columns && uriAt(end) == osc8) {
          ++end;
        }
        return;
      }
      for (int x = 0; x < columns; ++x) {
        GhosttyGridRef ref{};
        GhosttyCell raw = 0;
        if (!refAt(x, ref) || ghostty_grid_ref_cell(&ref, &raw) != GHOSTTY_SUCCESS) {
          continue;
        }
        GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
        if (ghostty_cell_get(raw, GHOSTTY_CELL_DATA_WIDE, &wide) == GHOSTTY_SUCCESS && wide == GHOSTTY_CELL_WIDE_SPACER_TAIL) {
          cells[static_cast<std::size_t>(x)] = 0;
          continue;
        }
        std::uint32_t codepoints[4];
        std::size_t count = 0;
        if (ghostty_grid_ref_graphemes(&ref, codepoints, 4, &count) == GHOSTTY_SUCCESS && count > 0) {
          cells[static_cast<std::size_t>(x)] = codepoints[0];
        }
      }
    });
    if (!osc8.empty()) {
      constexpr std::string_view kSchemes[] = {"http://", "https://", "mailto:", "file://"};
      if (!std::ranges::any_of(kSchemes, [&](std::string_view s) { return osc8.starts_with(s); })) {
        return std::nullopt;
      }
      return HoverLink{.row = cell.y, .start = start, .end = end, .uri = std::move(osc8)};
    }
    const auto found = findLink(cells, cell.x, m_cwd);
    if (!found) {
      return std::nullopt;
    }
    return HoverLink{.row = cell.y, .start = found->start, .end = found->end, .uri = found->uri};
  }

  void TerminalView::setFindQuery(const std::string& query) {
    if (!std::holds_alternative<TerminalFind>(m_find)) {
      m_find.emplace<TerminalFind>();
    }
    m_session.withTerminal([&](GhosttyTerminal t) { std::get<TerminalFind>(m_find).setQuery(t, query); });
    m_marksDirty = true;
    scheduleRedraw(Redraw::All);
  }

  void TerminalView::findStep(bool older) {
    auto* find = std::get_if<TerminalFind>(&m_find);
    if (find == nullptr) {
      return;
    }
    m_session.withTerminal([&](GhosttyTerminal) { find->step(older); });
    scheduleRedraw(Redraw::All);
  }

  void TerminalView::closeFind() {
    if (!std::holds_alternative<TerminalFind>(m_find)) {
      return;
    }
    m_session.withTerminal([&](GhosttyTerminal) { m_find = FindClosed{}; });
    m_marksDirty = true;
    if (m_findRing != nullptr) {
      m_findRing->setVisible(false);
    }
    scheduleRedraw(Redraw::All);
  }

  void TerminalView::updateMarks() {
    if (m_scrollbar == nullptr) {
      return;
    }
    if (const auto* find = std::get_if<TerminalFind>(&m_find)) {
      m_scrollbar->setMarks(find->marks());
      m_scrollbar->setCurrentMark(find->currentMark(), colorSpecFromRole(ColorRole::TermFindRing));
      return;
    }
    const std::uint64_t total = m_scrollState.total;
    m_scrollbar->setMarks(m_session.withCommands([&](GhosttyTerminal, const CommandIndex& index) {
      std::vector<float> marks;
      for (const auto& entry : index.entries()) {
        if (const auto row = CommandIndex::row(entry.prompt.get(), GHOSTTY_POINT_TAG_SCREEN); row && total > 0) {
          marks.push_back(static_cast<float>(*row) / static_cast<float>(total));
        }
      }
      return marks;
    }));
    m_scrollbar->setCurrentMark(std::nullopt, {});
  }

  void TerminalView::updateLink(bool ctrl) {
    std::optional<HoverLink> next;
    if (ctrl && m_grid != nullptr && m_grid->columns() > 0 && m_grid->rows() > 0) {
      next = linkAt(cellAt(m_pointerX, m_pointerY));
    }
    const bool overPill = m_context != nullptr && m_context->hitTest(m_pointerX, m_pointerY).has_value();
    if (next == m_link && overPill == m_overPill) {
      return;
    }
    m_link = std::move(next);
    m_overPill = overPill;
    if (m_area != nullptr) {
      m_area->setCursorShape(m_link || m_overPill ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER : 0);
    }
    scheduleRedraw(Redraw::All);
  }

  void TerminalView::pointerButton(std::uint32_t button, float localX, float localY, bool pressed,
                                   std::uint32_t time) {
    if (m_grid == nullptr || m_grid->columns() <= 0 || m_grid->rows() <= 0) {
      return;
    }
    const GhosttyMouseButton mouse = button == BTN_LEFT     ? GHOSTTY_MOUSE_BUTTON_LEFT
                                     : button == BTN_MIDDLE ? GHOSTTY_MOUSE_BUTTON_MIDDLE
                                                            : GHOSTTY_MOUSE_BUTTON_RIGHT;
    if (button == BTN_LEFT && pressed && m_link) {
      if (m_host.openUri) {
        m_host.openUri(m_link->uri);
      }
      return;
    }
    if (button == BTN_LEFT && pressed) {
      if (const auto pill = m_context->hitTest(localX, localY)) {
        if (pill->pill == ContextRow::Hit::Pill::Branch) {
          if (TextClipboard* clipboard = Input::textClipboard(); clipboard != nullptr) {
            clipboard->setClipboardText(pill->value);
          }
        } else if (m_host.openUri) {
          m_host.openUri(fileUri(pill->value));
        }
        return;
      }
    }
    const std::uint32_t bit = 1U << mouse;
    const std::uint32_t mods = modifiers();
    // Shift bypasses mouse reporting so the user can still select; a reported press always gets its release.
    const bool reported = (m_buttonsReported & bit) != 0;
    if ((pressed && (mods & KeyMod::Shift) == 0) || (!pressed && reported)) {
      if (pressed) {
        m_buttonsReported |= bit;
      } else {
        m_buttonsReported &= ~bit;
      }
      if (reportMouse(pressed ? GHOSTTY_MOUSE_ACTION_PRESS : GHOSTTY_MOUSE_ACTION_RELEASE, mouse, localX, localY,
                      mods)) {
        return;
      }
      m_buttonsReported &= ~bit;
    }
    if (button != BTN_LEFT) {
      return;
    }
    if (pressed) {
      const GhosttyPointCoordinate cell = cellAt(localX, localY);
      const bool repeat = m_lastClick.count > 0 && time - m_lastClick.time <= 400 && cell.x == m_lastClick.cell.x
                          && cell.y == m_lastClick.cell.y;
      m_lastClick = {.time = time, .cell = cell, .count = repeat ? m_lastClick.count % 3 + 1 : 1};
      if (m_lastClick.count == 1) {
        m_drag = DragSelecting{.anchor = cell};
      } else {
        m_drag = DragIdle{};
        selectAround(cell, m_lastClick.count);
      }
      return;
    }
    const auto* drag = std::get_if<DragSelecting>(&m_drag);
    // A click without a drag clears the selection.
    if (drag != nullptr && !drag->moved) {
      clearSelection();
    }
    m_drag = DragIdle{};
  }

  void TerminalView::pointerMotion(float localX, float localY) {
    if (m_grid == nullptr || m_grid->columns() <= 0 || m_grid->rows() <= 0) {
      return;
    }
    m_pointerX = localX;
    m_pointerY = localY;
    updateLink((modifiers() & KeyMod::Ctrl) != 0);
    auto* drag = std::get_if<DragSelecting>(&m_drag);
    if (drag == nullptr) {
      const std::uint32_t mods = modifiers();
      const GhosttyPointCoordinate cell = cellAt(localX, localY);
      if ((mods & KeyMod::Shift) != 0 || (cell.x == m_motionCell.x && cell.y == m_motionCell.y)) {
        return;
      }
      GhosttyMouseButton held = GHOSTTY_MOUSE_BUTTON_UNKNOWN;
      for (const GhosttyMouseButton b : {GHOSTTY_MOUSE_BUTTON_LEFT, GHOSTTY_MOUSE_BUTTON_MIDDLE, GHOSTTY_MOUSE_BUTTON_RIGHT}) {
        if ((m_buttonsReported & (1U << b)) != 0) {
          held = b;
          break;
        }
      }
      if (reportMouse(GHOSTTY_MOUSE_ACTION_MOTION, held, localX, localY, mods)) {
        m_motionCell = cell;
      }
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

  bool TerminalView::reportMouse(GhosttyMouseAction action, GhosttyMouseButton button, float localX, float localY,
                                 std::uint32_t mods) {
    const bool tracking = m_session.withTerminal([&](GhosttyTerminal t) {
      bool on = false;
      if (t == nullptr || ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &on) != GHOSTTY_SUCCESS || !on) {
        return false;
      }
      ghostty_mouse_encoder_set_from_terminal(m_mouseEncoder, t);
      return true;
    });
    if (!tracking) {
      return false;
    }
    auto px = [](float v) { return static_cast<std::uint32_t>(std::max(0L, std::lround(v))); };
    const std::uint32_t left = px(kInset * m_uiScale);
    const std::uint32_t top = px(kTopInset * m_uiScale);
    const std::uint32_t cellW = std::max(1U, px(m_grid->cellWidth()));
    const std::uint32_t cellH = std::max(1U, px(m_grid->cellHeight()));
    auto size = GHOSTTY_INIT_SIZED(GhosttyMouseEncoderSize);
    size.screen_width = px(m_width);
    size.screen_height = px(m_height);
    size.cell_width = cellW;
    size.cell_height = cellH;
    size.padding_left = left;
    size.padding_top = top;
    size.padding_right = px(m_width - static_cast<float>(left + cellW * static_cast<std::uint32_t>(m_grid->columns())));
    size.padding_bottom = px(m_height - static_cast<float>(top + cellH * static_cast<std::uint32_t>(viewRows())));
    ghostty_mouse_encoder_set(m_mouseEncoder, GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &size);
    const bool anyPressed = m_buttonsReported != 0;
    ghostty_mouse_encoder_set(m_mouseEncoder, GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED, &anyPressed);
    ghostty_mouse_event_set_action(m_mouseEvent, action);
    if (button == GHOSTTY_MOUSE_BUTTON_UNKNOWN) {
      ghostty_mouse_event_clear_button(m_mouseEvent);
    } else {
      ghostty_mouse_event_set_button(m_mouseEvent, button);
    }
    ghostty_mouse_event_set_mods(m_mouseEvent, ghosttyMods(mods));
    ghostty_mouse_event_set_position(m_mouseEvent, {.x = localX, .y = localY});
    std::uint8_t buf[64];
    std::size_t written = 0;
    if (ghostty_mouse_encoder_encode(m_mouseEncoder, m_mouseEvent, buf, sizeof(buf), &written) == GHOSTTY_SUCCESS
        && written > 0) {
      m_session.write({buf, written});
    }
    return true;
  }

  bool TerminalView::mouseTracking() {
    return m_session.withTerminal([](GhosttyTerminal t) {
      bool on = false;
      return t != nullptr && ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &on) == GHOSTTY_SUCCESS && on;
    });
  }

  bool TerminalView::wheel(const InputArea::PointerData& d) {
    if (d.axis != WL_POINTER_AXIS_VERTICAL_SCROLL || m_grid == nullptr) {
      return false;
    }
    m_wheelRemainder += d.axisSteps;
    const int notches = static_cast<int>(m_wheelRemainder);
    m_wheelRemainder -= static_cast<float>(notches);
    const std::uint32_t mods = modifiers();
    if ((mods & KeyMod::Shift) == 0 && mouseTracking()) {
      const GhosttyMouseButton button = notches < 0 ? GHOSTTY_MOUSE_BUTTON_FOUR : GHOSTTY_MOUSE_BUTTON_FIVE;
      for (int i = 0; i < std::abs(notches); ++i) {
        reportMouse(GHOSTTY_MOUSE_ACTION_PRESS, button, m_pointerX, m_pointerY, mods);
      }
      return true;
    }
    constexpr int kRowsPerNotch = 3;
    // The alternate screen has no scrollback: with mode 1007 the wheel becomes arrow keys (less, man, vim).
    const auto alternate = m_session.withTerminal([&](GhosttyTerminal t) -> std::optional<std::string> {
      GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
      GhosttyTerminalModeConfig alternateScroll{.mode = GHOSTTY_MODE_MOUSE_ALTERNATE_SCROLL, .value = false};
      GhosttyTerminalModeConfig cursorKeys{.mode = GHOSTTY_MODE_CURSOR_KEYS, .value = false};
      if (t == nullptr || ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &screen) != GHOSTTY_SUCCESS
          || screen != GHOSTTY_TERMINAL_SCREEN_ALTERNATE) {
        return std::nullopt;
      }
      std::string out;
      if (ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_MODE, &alternateScroll) == GHOSTTY_SUCCESS
          && alternateScroll.value) {
        ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_MODE, &cursorKeys);
        for (int i = 0; i < std::abs(notches) * kRowsPerNotch; ++i) {
          out += cursorKeys.value ? "\x1bO" : "\x1b[";
          out += notches < 0 ? 'A' : 'B';
        }
      }
      return out;
    });
    if (alternate) {
      if (!alternate->empty()) {
        m_session.write({reinterpret_cast<const std::uint8_t*>(alternate->data()), alternate->size()});
      }
      return true;
    }
    const float cellH = m_grid->cellHeight();
    if (d.axisSource == WL_POINTER_AXIS_SOURCE_FINGER) {
      if (m_scrollMotion == ScrollMotion::Settling && m_area->animationManager() != nullptr) {
        m_area->animationManager()->cancel(m_scrollAnim);
        m_scrollAnim = 0;
      }
      if (m_scrollMotion != ScrollMotion::Finger) {
        m_scrollSamples.clear();
      }
      m_scrollMotion = ScrollMotion::Finger;
      setScrollPx(m_scrollPx + d.scrollDelta(cellH * kRowsPerNotch));
      m_scrollSamples.push_back({.time = d.time, .px = m_scrollPx});
      std::erase_if(m_scrollSamples, [&](const ScrollSample& s) { return d.time - s.time > 100; });
      return true;
    }
    if (notches != 0) {
      const float from = m_scrollMotion == ScrollMotion::Settling ? m_scrollTarget : std::round(m_scrollPx / cellH) * cellH;
      animateScroll(from + static_cast<float>(notches * kRowsPerNotch) * cellH, Tokens::springMove, 0.0F, true);
    }
    return true;
  }

  void TerminalView::setScrollPx(float px) {
    const float cellH = m_grid->cellHeight();
    const float maxPx = static_cast<float>(m_scrollState.total - m_scrollState.len) * cellH;
    m_scrollPx = std::clamp(px, 0.0F, std::max(0.0F, maxPx));
    const auto row = static_cast<std::uint64_t>(m_scrollPx / cellH);
    if (row != m_viewRow) {
      m_viewRow = row;
      m_session.withTerminal([&](GhosttyTerminal t) {
        if (t != nullptr) {
          ghostty_terminal_scroll_viewport(
              t, {.tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_ROW, .value = {.row = static_cast<std::size_t>(row)}});
        }
      });
      scheduleRedraw(Redraw::All);
    }
    setGridOrigin(kInset * m_uiScale, kTopInset * m_uiScale - (m_scrollPx - static_cast<float>(row) * cellH));
    if (m_scrollbar != nullptr) {
      m_scrollbar->update(static_cast<float>(m_scrollState.len) * cellH, static_cast<float>(m_scrollState.total) * cellH,
                          m_scrollPx);
    }
  }

  // Springs toward `target`, snapped to a whole row; a later wheel notch or fling retargets the running spring.
  void TerminalView::animateScroll(float target, const Tokens::Spring& spring, float velocity, bool spatial) {
    const float cellH = m_grid->cellHeight();
    const float maxPx = static_cast<float>(m_scrollState.total - m_scrollState.len) * cellH;
    m_scrollTarget = std::clamp(std::round(target / cellH) * cellH, 0.0F, std::max(0.0F, maxPx));
    m_scrollSamples.clear();
    AnimationManager* animations = m_area != nullptr ? m_area->animationManager() : nullptr;
    m_scrollMotion = ScrollMotion::Settling;
    if (animations != nullptr) {
      m_scrollAnim = animations->drive(
          m_scrollAnim, m_scrollPx, m_scrollTarget, Style::spring(spring), [this](float v) { setScrollPx(v); },
          [this] {
            m_scrollAnim = 0;
            m_scrollMotion = ScrollMotion::Rest;
          },
          this, velocity, Unit::Px, spatial);
    }
    if (animations == nullptr || m_scrollAnim == 0) {
      m_scrollAnim = 0;
      setScrollPx(m_scrollTarget);
      m_scrollMotion = ScrollMotion::Rest;
    }
  }

  // Rulebook 7.7: project the finger's velocity, snap to the nearest row, settle on `page` with that velocity.
  // Reduced motion (7.8): settle on `close` from the current position, no velocity carry.
  void TerminalView::settleScroll() {
    if (MotionService::instance().reduced()) {
      animateScroll(m_scrollPx, Tokens::springClose, 0.0F, false);
      return;
    }
    const float v = fingerVelocity();
    constexpr float kDeceleration = 0.998F;
    animateScroll(m_scrollPx + v * kDeceleration / (1.0F - kDeceleration) / 1000.0F, Tokens::springPage, v, true);
  }

  // px/s: least squares over the samples of the last 100 ms.
  float TerminalView::fingerVelocity() const {
    if (m_scrollSamples.size() < 2) {
      return 0.0F;
    }
    double meanT = 0.0;
    double meanP = 0.0;
    const std::uint32_t t0 = m_scrollSamples.front().time;
    for (const ScrollSample& s : m_scrollSamples) {
      meanT += static_cast<double>(s.time - t0);
      meanP += s.px;
    }
    const auto n = static_cast<double>(m_scrollSamples.size());
    meanT /= n;
    meanP /= n;
    double num = 0.0;
    double den = 0.0;
    for (const ScrollSample& s : m_scrollSamples) {
      const double dt = static_cast<double>(s.time - t0) - meanT;
      num += dt * (s.px - meanP);
      den += dt * dt;
    }
    return den > 0.0 ? static_cast<float>(num / den * 1000.0) : 0.0F;
  }

  void TerminalView::selectAround(GhosttyPointCoordinate cell, int count) {
    const bool set = m_session.withTerminal([&](GhosttyTerminal t) {
      GhosttyGridRef ref{};
      GhosttySelection selection{};
      if (t == nullptr
          || ghostty_terminal_grid_ref(t, {.tag = GHOSTTY_POINT_TAG_VIEWPORT, .value = {.coordinate = cell}}, &ref)
                 != GHOSTTY_SUCCESS) {
        return false;
      }
      GhosttyResult result = GHOSTTY_NO_VALUE;
      if (count == 2) {
        auto options = GHOSTTY_INIT_SIZED(GhosttyTerminalSelectWordOptions);
        options.ref = ref;
        result = ghostty_terminal_select_word(t, &options, &selection);
      } else {
        auto options = GHOSTTY_INIT_SIZED(GhosttyTerminalSelectLineOptions);
        options.ref = ref;
        result = ghostty_terminal_select_line(t, &options, &selection);
      }
      return result == GHOSTTY_SUCCESS
             && ghostty_terminal_set(t, GHOSTTY_TERMINAL_OPT_SELECTION, &selection) == GHOSTTY_SUCCESS;
    });
    if (set) {
      scheduleRedraw(Redraw::All);
    }
  }

  void TerminalView::clearSelection() {
    m_session.withTerminal([](GhosttyTerminal t) {
      if (t != nullptr) {
        ghostty_terminal_set(t, GHOSTTY_TERMINAL_OPT_SELECTION, nullptr);
      }
    });
    scheduleRedraw(Redraw::All);
  }

  // Typing or pasting returns a scrolled-back view to the live screen.
  void TerminalView::scrollToBottom() {
    const bool moved = m_session.withTerminal([](GhosttyTerminal t) {
      bool active = true;
      return t != nullptr && ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_VIEWPORT_ACTIVE, &active) == GHOSTTY_SUCCESS
             && !active
             && ghostty_terminal_scroll_viewport(t, {.tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_BOTTOM, .value = {}})
                    == GHOSTTY_SUCCESS;
    });
    if (moved) {
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
      scrollToBottom();
      m_session.write({buf.data(), written});
    }
  }

} // namespace term
