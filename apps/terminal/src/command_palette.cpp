#include "command_palette.h"

#include "render/animation/animation_manager.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <unordered_map>
#include <utility>

namespace term {

  namespace {

    constexpr std::uint32_t kUp = 0xff52;
    constexpr std::uint32_t kDown = 0xff54;
    constexpr std::uint32_t kEscape = 0xff1b;
    constexpr std::uint32_t kReturn = 0xff0d;
    constexpr std::uint32_t kKeypadEnter = 0xff8d;
    constexpr std::uint32_t kTab = 0xff09;
    constexpr std::uint32_t kLeftTab = 0xfe20;
    constexpr float kGrowFrom = 0.92F; // the Grow motion's start scale (context menus use the same)

    void escapeInto(std::string& out, std::string_view text) {
      for (const char c : text) {
        switch (c) {
          case '&': out += "&amp;"; break;
          case '<': out += "&lt;"; break;
          case '>': out += "&gt;"; break;
          default: out += c;
        }
      }
    }

    // Pango markup: the codepoints at `hits` in label_1 600, the rest in the
    // label's own colour.
    std::string marked(std::string_view text, const std::vector<std::uint32_t>& hits) {
      if (hits.empty()) {
        std::string out;
        escapeInto(out, text);
        return out;
      }
      const Color c = colorForRole(ColorRole::Label1);
      const auto byte = [](float v) { return static_cast<unsigned>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F)); };
      const std::string open = std::format(R"(<span weight="600" foreground="#{:02x}{:02x}{:02x}" fgalpha="{}%">)",
                                           byte(c.r), byte(c.g), byte(c.b), std::lround(c.a * 100.0F));
      std::string out;
      std::uint32_t index = 0;
      std::size_t next = 0;
      for (std::size_t i = 0; i < text.size(); ++index) {
        std::size_t len = 1;
        while (i + len < text.size() && (static_cast<unsigned char>(text[i + len]) & 0xC0) == 0x80) {
          ++len;
        }
        const bool hit = next < hits.size() && hits[next] == index;
        if (hit) {
          ++next;
          out += open;
        }
        escapeInto(out, text.substr(i, len));
        if (hit) {
          out += "</span>";
        }
        i += len;
      }
      return out;
    }

  } // namespace

  CommandPalette::CommandPalette(AnimationManager& animations, std::vector<palette::Item> items,
                                 std::vector<std::string> recent, Host host)
      : m_animations(animations), m_items(std::move(items)), m_recent(std::move(recent)), m_host(std::move(host)) {}

  InputArea* CommandPalette::focusArea() const { return m_input->inputArea(); }

  std::unique_ptr<Node> CommandPalette::build(float scale, float slotWidth, float slotHeight, Rect dim, Rect dock) {
    const float s = m_scale = scale;
    const float inset = Tokens::spaceP8 * s;
    auto root = ui::node({.width = slotWidth, .height = slotHeight, .participatesInLayout = false});
    m_root = root.get();
    auto shade = ui::box({.fill = colorSpecFromRole(ColorRole::Shadow, Tokens::componentTerminalPaletteDim),
                          .width = dim.width,
                          .height = dim.height});
    shade->setPosition(dim.x, dim.y);
    root->addChild(std::move(shade));

    const float stackWidth = dock.width - 2.0F * inset;
    m_rowWidth = stackWidth - 2.0F * Tokens::spaceP4 * s;
    auto stack = ui::column({.out = &m_stack,
                             .align = FlexAlign::Stretch,
                             .justify = FlexJustify::End,
                             .gap = Tokens::spaceP4 * s,
                             .width = stackWidth,
                             .height = dock.height - 2.0F * inset});
    stack->setPosition(dock.x + inset, dock.y + inset);
    const auto shadow = [&](Box** out) {
      auto box =
          ui::box({.out = out,
                   .fill = colorSpecFromRole(ColorRole::Shadow, isResolvedLightTheme() ? Tokens::elevationE3.keyLight
                                                                                       : Tokens::elevationE3.keyDark),
                   .participatesInLayout = false});
      box->setShadow(Tokens::elevationE3.keyBlur * s, Tokens::elevationE3.keyY * s);
      box->setZIndex(-1);
      return box;
    };
    stack->addChild(shadow(&m_resultsShadow));
    stack->addChild(shadow(&m_lineShadow));

    auto results = ui::column({.out = &m_results,
                               .align = FlexAlign::Stretch,
                               .padding = Tokens::spaceP4 * s,
                               .fill = colorSpecFromRole(ColorRole::Card),
                               .radius = Style::scaledRadius(Tokens::radiusR12, s),
                               .border = colorSpecFromRole(ColorRole::Stroke),
                               .borderWidth = 0.5F * s});
    const float rowRadius = Style::scaledRadius(Tokens::radiusR8, s);
    results->addChild(ui::box({.out = &m_lens,
                               .fill = colorSpecFromRole(ColorRole::Lens),
                               .radius = rowRadius,
                               .width = m_rowWidth,
                               .height = Tokens::sizeRowCompact * s,
                               .visible = false,
                               .participatesInLayout = false}));
    results->addChild(ui::box({.out = &m_lensEdge,
                               .fill = colorSpecFromRole(ColorRole::LensEdge),
                               .width = m_rowWidth - 2.0F * rowRadius,
                               .height = 0.5F * s,
                               .visible = false,
                               .participatesInLayout = false}));
    results->addChild(ui::column({.out = &m_rows, .align = FlexAlign::Stretch}));
    stack->addChild(std::move(results));

    const float lineHeight = Tokens::sizeControlL * s;
    auto line = ui::row({.out = &m_line,
                         .align = FlexAlign::Center,
                         .gap = Tokens::spaceP8 * s,
                         .paddingH = Tokens::spaceP12 * s,
                         .fill = colorSpecFromRole(ColorRole::Card),
                         .radius = rowRadius,
                         .border = colorSpecFromRole(ColorRole::Stroke),
                         .borderWidth = 0.5F * s,
                         .minHeight = lineHeight,
                         .maxHeight = lineHeight});
    line->addChild(ui::glyph({.glyph = std::string("chevron-right"),
                              .glyphSize = Tokens::sizeSymbol * s,
                              .color = colorSpecFromRole(ColorRole::Label2)}));
    line->addChild(ui::input({.out = &m_input,
                              .placeholder = std::string("Session, action or theme"),
                              .fontSize = Tokens::typeTitle3.size * s,
                              .horizontalPadding = 0.0F,
                              .frameVisible = false,
                              .fontWeight = FontWeight::Normal,
                              .flexGrow = 1.0F,
                              .onChange =
                                  [this](const std::string& text) {
                                    m_query = text;
                                    filter();
                                    fillRows();
                                    if (!m_detached) {
                                      m_host.relayout();
                                    }
                                  },
                              .onKeyEvent = [this](std::uint32_t sym, std::uint32_t mods) { return key(sym, mods); },
                              .onFocusLoss =
                                  [this] {
                                    if (!m_detached) {
                                      m_host.close();
                                    }
                                  }}));
    line->addChild(ui::keycap({.text = "esc", .scale = s}));
    stack->addChild(std::move(line));
    root->addChild(std::move(stack));

    filter();
    fillRows();
    return root;
  }

  void CommandPalette::filter() {
    m_list.clear();
    m_recentCount = 0;
    if (m_query.empty()) {
      std::unordered_map<std::string_view, std::size_t> byId;
      for (std::size_t i = 0; i < m_items.size(); ++i) {
        byId.emplace(m_items[i].id, i);
      }
      for (const std::string& id : m_recent) {
        if (const auto it = byId.find(id); it != byId.end() && m_list.size() < 3) {
          m_list.push_back({.item = it->second});
        }
      }
      m_recentCount = m_list.size();
      // Other sessions, in sidebar order from the top; m_list runs from the line
      // upward.
      std::vector<palette::Match> sessions;
      for (std::size_t i = 0; i < m_items.size() && sessions.size() < 4; ++i) {
        if (m_items[i].kind == palette::Kind::Session && m_items[i].trailing != "this pane" &&
            !std::ranges::contains(m_list, i, &palette::Match::item)) {
          sessions.push_back({.item = i});
        }
      }
      m_list.insert(m_list.end(), sessions.rbegin(), sessions.rend());
    } else {
      m_list = palette::rank(m_query, m_items);
    }
    m_offset = 0;
    setCurrent(0);
  }

  void CommandPalette::setCurrent(std::size_t index) {
    m_current = index;
    const auto rows = static_cast<std::size_t>(Tokens::componentTerminalPaletteRows);
    if (m_current < m_offset) {
      m_offset = m_current;
    } else if (m_current >= m_offset + rows) {
      m_offset = m_current + 1 - rows;
    }
    if (!m_detached) {
      m_host.current(m_current < m_list.size() ? &m_items[m_list[m_current].item] : nullptr);
    }
  }

  void CommandPalette::fillRows() {
    const float s = m_scale;
    const float header = Tokens::componentTerminalPaletteHeader * s;
    std::vector<std::unique_ptr<Node>> removed;
    while (!m_rows->children().empty()) {
      removed.push_back(m_rows->removeChild(m_rows->children().back().get()));
    }
    const auto footnote = [&](std::string text, FontWeight weight) {
      return ui::label({.text = std::move(text),
                        .fontSize = Tokens::typeFootnote.size * s,
                        .fontWeight = weight,
                        .color = colorSpecFromRole(ColorRole::Label2),
                        .maxLines = 1});
    };
    const auto headerLine = [&](std::string text, FontWeight weight) {
      auto lineRow = ui::row(
          {.align = FlexAlign::Center, .paddingH = Tokens::spaceP8 * s, .minHeight = header, .maxHeight = header});
      lineRow->addChild(footnote(std::move(text), weight));
      m_rows->addChild(std::move(lineRow));
    };
    float y = Tokens::spaceP4 * s;
    m_lensShown = false;
    if (m_list.empty()) {
      m_results->setVisible(!m_query.empty());
      if (!m_query.empty()) {
        auto empty = ui::column({.align = FlexAlign::Start,
                                 .gap = Tokens::spaceP4 * s,
                                 .paddingV = Tokens::spaceP12 * s,
                                 .paddingH = Tokens::spaceP8 * s});
        empty->addChild(ui::label({.text = std::format("No session, action or theme matches “{}”", m_query),
                                   .fontSize = Tokens::typeBody.size * s,
                                   .color = colorSpecFromRole(ColorRole::Label1),
                                   .maxLines = 1}));
        empty->addChild(footnote(std::format("Enter opens a new session named “{}”", m_query), FontWeight::Normal));
        m_rows->addChild(std::move(empty));
      }
    } else {
      m_results->setVisible(true);
      const auto rows = static_cast<std::size_t>(Tokens::componentTerminalPaletteRows);
      const std::size_t end = std::min(m_list.size(), m_offset + rows);
      if (const std::size_t more = m_list.size() - end; more > 0) {
        headerLine(std::format("{} more · ↑ scrolls", more), FontWeight::Normal);
        y += header;
      }
      for (std::size_t i = end; i-- > m_offset;) {
        if (m_query.empty() && (i + 1 == m_list.size() || i + 1 == m_recentCount)) {
          headerLine(i >= m_recentCount ? "Sessions" : "Recent", FontWeight::SemiBold);
          y += header;
        }
        if (i == m_current) {
          m_lensTarget = y;
          m_lensShown = true;
        }
        m_rows->addChild(makeRow(i, i == m_current));
        y += Tokens::sizeRowCompact * s;
      }
    }
    removed.clear();
  }

  std::unique_ptr<Flex> CommandPalette::makeRow(std::size_t index, bool current) {
    const float s = m_scale;
    const palette::Match& match = m_list[index];
    const palette::Item& item = m_items[match.item];
    const float height = Tokens::sizeRowCompact * s;
    auto row = ui::row({.align = FlexAlign::Center,
                        .gap = Tokens::spaceP8 * s,
                        .paddingH = Tokens::spaceP8 * s,
                        .minHeight = height,
                        .maxHeight = height,
                        .opacity = item.off.empty() ? 1.0F : Tokens::componentTerminalPaletteOffOpacity});
    const float symbol = Tokens::sizeSymbol * s;
    if (item.kind == palette::Kind::Theme) {
      auto swatch = ui::row({.align = FlexAlign::Center, .gap = 2.0F * s, .width = symbol});
      for (const std::uint32_t colour : item.swatch) {
        swatch->addChild(ui::box(
            {.fill = fixedColorSpec(rgbaHex(colour)), .radius = 2.0F * s, .width = 4.0F * s, .height = 12.0F * s}));
      }
      row->addChild(std::move(swatch));
    } else {
      const ColorRole role = current ? ColorRole::AccentText : item.danger ? ColorRole::DangerText : ColorRole::Label2;
      row->addChild(ui::glyph({.glyph = item.glyph, .glyphSize = symbol, .color = colorSpecFromRole(role)}));
    }
    row->addChild(ui::label({.text = current ? marked(item.label, {}) : marked(item.label, match.hits),
                             .fontSize = Tokens::typeBody.size * s,
                             .fontWeight = current ? FontWeight::SemiBold : FontWeight::Normal,
                             .color = colorSpecFromRole(current ? ColorRole::Label1 : ColorRole::Label2),
                             .maxWidth = m_rowWidth * 0.6F,
                             .maxLines = 1,
                             .configure = [](Label& label) { label.setUseMarkup(true); }}));
    row->addChild(ui::label({.text = marked(item.sub, match.subHits),
                             .fontSize = Tokens::typeFootnote.size * s,
                             .color = colorSpecFromRole(ColorRole::Label2),
                             .minWidth = 0.0F,
                             .maxLines = 1,
                             .flexGrow = 1.0F,
                             .configure = [](Label& label) { label.setUseMarkup(true); }}));
    const auto endText = [&](std::string text) {
      row->addChild(ui::label({.text = std::move(text),
                               .fontSize = Tokens::typeFootnote.size * s,
                               .color = colorSpecFromRole(ColorRole::Label2),
                               .maxLines = 1}));
    };
    if (current) {
      endText(item.kind == palette::Kind::Session ? "Show ↩" : item.kind == palette::Kind::Theme ? "Apply ↩" : "Run ↩");
    } else if (!item.off.empty()) {
      endText(item.off);
    } else if (!item.keys.empty()) {
      auto caps = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s});
      for (const std::string& cap : item.keys) {
        caps->addChild(ui::keycap({.text = cap, .scale = s}));
      }
      row->addChild(std::move(caps));
    } else if (!item.trailing.empty()) {
      endText(item.trailing);
    }
    row->addChild(
        ui::inputArea({.width = m_rowWidth,
                       .height = height,
                       .participatesInLayout = false,
                       .onClick = [this, index](const InputArea::PointerData& /*pointer*/) { pick(index); }}));
    return row;
  }

  void CommandPalette::afterLayout(bool animate) {
    const auto place = [](Box* shadow, const Flex* card, float s) {
      shadow->setVisible(card->visible());
      shadow->setRadius(card->radius());
      shadow->setSize(card->width(), card->height());
      shadow->setPosition(card->x(), card->y() + Tokens::elevationE3.keyY * s);
    };
    place(m_resultsShadow, m_results, m_scale);
    place(m_lineShadow, m_line, m_scale);
    const float x = Tokens::spaceP4 * m_scale;
    m_lens->setVisible(m_lensShown);
    m_lensEdge->setVisible(m_lensShown);
    const auto set = [this, x](float y) {
      m_lensY = y;
      m_lens->setPosition(x, y);
      m_lensEdge->setPosition(x + (m_rowWidth - m_lensEdge->width()) / 2.0F, y);
    };
    if (!m_lensShown) {
      return;
    }
    if (animate && std::exchange(m_travel, false) && m_lensY != m_lensTarget) {
      m_lensAnim = m_animations.animateSpatial(
          m_lensY, m_lensTarget, Style::spring(Tokens::springSnap), set, [this] { m_lensAnim = 0; }, m_lens, 0.0F,
          Unit::Px);
    } else {
      if (m_lensAnim != 0) {
        m_animations.cancel(m_lensAnim);
        m_lensAnim = 0;
      }
      set(m_lensTarget);
    }
  }

  void CommandPalette::grow() {
    m_root->setOpacity(0.0F);
    m_stack->setTransformOrigin(m_stack->width() / 2.0F, m_stack->height());
    m_stack->setScale(kGrowFrom);
    m_animations.animateSpatial(
        kGrowFrom, 1.0F, Style::spring(Tokens::springOpen), [this](float v) { m_stack->setScale(v); }, {}, m_stack);
    m_animations.animateSpring(
        0.0F, 1.0F, Style::spring(Tokens::springFade), [this](float v) { m_root->setOpacity(v); }, {}, m_root);
  }

  // A host callback may detach the palette while it runs: the callbacks stay,
  // only the calls stop.
  void CommandPalette::detach() { m_detached = true; }

  void CommandPalette::shrink(std::function<void()> done) {
    detach();
    m_animations.animateSpring(
        m_root->opacity(), 0.0F, Style::spring(Tokens::springFadeOut), [this](float v) { m_root->setOpacity(v); },
        std::move(done), m_root);
  }

  void CommandPalette::moveCurrent(int by) {
    if (m_list.empty()) {
      return;
    }
    const auto last = static_cast<std::ptrdiff_t>(m_list.size()) - 1;
    const auto next = std::clamp(static_cast<std::ptrdiff_t>(m_current) + by, std::ptrdiff_t{0}, last);
    if (static_cast<std::size_t>(next) == m_current) {
      return;
    }
    setCurrent(static_cast<std::size_t>(next));
    m_travel = true;
    fillRows();
    if (!m_detached) {
      m_host.relayout();
    }
  }

  bool CommandPalette::key(std::uint32_t sym, std::uint32_t /*mods*/) {
    switch (sym) {
      case kUp: moveCurrent(1); return true; // the list reads upward from the line
      case kDown: moveCurrent(-1); return true;
      case kEscape:
        if (!m_detached) {
          m_host.close();
        }
        return true;
      case kReturn:
      case kKeypadEnter:
        if (m_list.empty()) {
          if (!m_query.empty()) {
            if (!m_detached) {
              m_host.runNoMatch(m_query);
            }
          }
        } else {
          pick(m_current);
        }
        return true;
      case kTab:
      case kLeftTab: return true; // focus stays in the field while the palette is open
      default: return false;
    }
  }

  void CommandPalette::pick(std::size_t index) {
    if (index < m_list.size() && m_items[m_list[index].item].off.empty()) {
      if (!m_detached) {
        m_host.run(m_items[m_list[index].item]);
      }
    }
  }

} // namespace term
