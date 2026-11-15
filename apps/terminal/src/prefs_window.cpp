#include "prefs_window.h"

#include "app/app_window.h"
#include "cursor-shape-v1-client-protocol.h"
#include "core/deferred_call.h"
#include "design_tokens.h"
#include "terminal_view.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>

namespace term {

  namespace {

    constexpr std::string_view kCursorHint = "Programs that choose their own cursor keep it";

    struct PageInfo {
      std::string_view title;
      std::string_view glyph;
    };

    constexpr std::array<PageInfo, 6> kPages{{{"Appearance", "contrast"},
                                             {"Text", "typography"},
                                             {"Terminal", "terminal-2"},
                                             {"Sessions", "list-details"},
                                             {"Notifications", "bell"},
                                             {"Keyboard", "keyboard"}}};

    std::string lower(std::string_view text) {
      std::string out(text);
      std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return out;
    }

    std::vector<std::string> monoFamilies() {
      std::set<std::string> seen;
      FcPattern* pattern = FcPatternBuild(nullptr, FC_SPACING, FcTypeInteger, FC_MONO, nullptr);
      FcObjectSet* objects = FcObjectSetBuild(FC_FAMILY, nullptr);
      if (FcFontSet* fonts = FcFontList(nullptr, pattern, objects); fonts != nullptr) {
        for (int i = 0; i < fonts->nfont; ++i) {
          FcChar8* family = nullptr;
          if (FcPatternGetString(fonts->fonts[i], FC_FAMILY, 0, &family) == FcResultMatch) {
            seen.insert(reinterpret_cast<const char*>(family));
          }
        }
        FcFontSetDestroy(fonts);
      }
      FcObjectSetDestroy(objects);
      FcPatternDestroy(pattern);
      std::vector<std::string> out(seen.begin(), seen.end());
      std::ranges::sort(out, [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
      return out;
    }

    // /etc/shells without duplicates through symlinks (/bin/zsh and /usr/bin/zsh).
    std::vector<std::string> installedShells() {
      std::vector<std::string> out;
      std::set<std::filesystem::path> seen;
      std::ifstream in("/etc/shells");
      for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] != '/') {
          continue;
        }
        std::error_code ec;
        const auto real = std::filesystem::canonical(line, ec);
        if (!ec && seen.insert(real).second) {
          out.push_back(line);
        }
      }
      return out;
    }

    std::string tildePath(const std::filesystem::path& path) {
      const char* home = std::getenv("HOME");
      const std::string text = path.string();
      if (home != nullptr && *home != '\0' && text.starts_with(home)) {
        return "~" + text.substr(std::string_view(home).size());
      }
      return text;
    }

    std::unique_ptr<Node> keys(std::string text, float s) {
      return ui::label({.text = std::move(text),
                        .fontSize = Tokens::typeBody.size * s,
                        .color = colorSpecFromRole(ColorRole::Label2)});
    }

  } // namespace

  PrefsWindow::PrefsWindow(AppWindow& window, SettingsStore& store)
      : m_window(window), m_store(store), m_monoFonts(monoFamilies()), m_shells(installedShells()) {
    m_window.setTitle(std::string(kPages[0].title));
    m_window.setSidebarBuilder([this](Renderer&, float width, float) { return buildSidebar(width); });
    m_window.setContentBuilder([this](Renderer&, float width, float height) -> std::unique_ptr<Node> {
      auto slot = ui::column({.align = FlexAlign::Stretch, .width = width, .height = height});
      m_slot = slot.get();
      m_slotWidth = width;
      m_slotHeight = height;
      slot->addChild(buildPage(width, height));
      return slot;
    });
    // A control's own callback writes the file: rebuild after it returns, not under it.
    m_storeChanged = m_store.changed().connect([this, alive = std::weak_ptr<bool>(m_alive)] {
      DeferredCall::callLater([this, alive] {
        if (!alive.expired()) {
          refill();
        }
      });
    });
  }

  void PrefsWindow::change(const std::function<void(Settings&)>& edit) {
    Settings next = m_store.settings();
    edit(next);
    if (next != m_store.settings()) {
      m_store.update(next);
    }
  }

  void PrefsWindow::showPage(Page page) {
    m_page = page;
    m_query.clear();
    m_scroll = {};
    m_window.setTitle(std::string(kPages[static_cast<std::size_t>(page)].title));
    m_window.requestRebuild();
  }

  void PrefsWindow::setQuery(std::string query) {
    m_query = std::move(query);
    m_scroll = {};
    refill();
  }

  void PrefsWindow::refill() {
    if (m_slot == nullptr) {
      return;
    }
    m_window.updateInPlace([this](Renderer&) {
      std::vector<std::unique_ptr<Node>> removed;
      while (!m_slot->children().empty()) {
        removed.push_back(m_slot->removeChild(m_slot->children().back().get()));
      }
      m_window.input().forgetDetached(); // while the removed areas still live
      removed.clear();
      m_slot->addChild(buildPage(m_slotWidth, m_slotHeight));
      m_slot->markLayoutDirty();
    });
  }

  std::unique_ptr<Node> PrefsWindow::buildSidebar(float width) {
    const float s = m_window.uiScale();
    auto side = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP2 * s, .padding = Tokens::spaceP8 * s,
                            .width = width});
    auto search = ui::searchCapsule({
        .placeholder = "Search settings",
        .height = Tokens::sizeControlL,
        .scale = s,
        .value = m_query,
        .onChange = [this](const std::string& text) { setQuery(text); },
    });
    side->addChild(ui::column({.align = FlexAlign::Stretch, .paddingV = Tokens::spaceP4 * s}, std::move(search)));
    for (std::size_t i = 0; i < kPages.size(); ++i) {
      const auto page = static_cast<Page>(i);
      const bool current = page == m_page;
      side->addChild(ui::button({
          .text = std::string(kPages[i].title),
          .glyph = std::string(kPages[i].glyph),
          .glyphSize = Tokens::sizeSymbol * s,
          .selected = current,
          .contentAlign = ButtonContentAlign::Start,
          .variant = current ? ButtonVariant::TabActive : ButtonVariant::Ghost,
          .radius = Style::scaledRadiusSm(s),
          .height = Tokens::sizeControlM * s,
          .onClick = [this, page] { showPage(page); },
      }));
    }
    return side;
  }

  std::vector<PrefsWindow::Group> PrefsWindow::groups() const {
    const Settings& now = m_store.settings();
    std::vector<Group> out;
    auto* self = const_cast<PrefsWindow*>(this);

    const auto toggle = [self](bool value, std::function<void(Settings&, bool)> set, bool enabled = true) {
      return [self, value, set = std::move(set), enabled](float s, float) -> std::unique_ptr<Node> {
        return ui::toggle({.checked = value,
                           .enabled = enabled,
                           .scale = s,
                           .onChange = [self, set](bool on) { self->change([&](Settings& x) { set(x, on); }); }});
      };
    };
    const auto stepper = [self](int value, int min, int max, std::string suffix, std::function<void(Settings&, int)> set,
                                bool enabled = true) {
      return [self, value, min, max, suffix = std::move(suffix), set = std::move(set),
              enabled](float s, float) -> std::unique_ptr<Node> {
        return ui::stepper({.minValue = min,
                            .maxValue = max,
                            .step = 1,
                            .value = value,
                            .enabled = enabled,
                            .scale = s,
                            .valueSuffix = suffix,
                            .onValueChanged = [self, set](int v) { self->change([&](Settings& x) { set(x, v); }); }});
      };
    };
    const auto segmented = [self](std::vector<std::string> labels, std::size_t selected,
                                  std::function<void(Settings&, std::size_t)> set) {
      return [self, labels = std::move(labels), selected, set = std::move(set)](float s, float) -> std::unique_ptr<Node> {
        std::vector<ui::SegmentedOption> options;
        for (const auto& label : labels) {
          options.push_back({.label = label});
        }
        return ui::segmented({.options = std::move(options),
                              .selectedIndex = selected,
                              .scale = s,
                              .onChange = [self, set](std::size_t i) { self->change([&](Settings& x) { set(x, i); }); }});
      };
    };
    const auto select = [self](std::vector<std::string> options, std::size_t selected,
                               std::function<void(Settings&, std::size_t)> set) {
      return [self, options = std::move(options), selected, set = std::move(set)](float, float) -> std::unique_ptr<Node> {
        return ui::select({.options = options,
                           .selectedIndex = selected,
                           .onSelectionChanged = [self, set](std::size_t i, std::string_view) {
                             self->change([&](Settings& x) { set(x, i); });
                           }});
      };
    };

    // Appearance
    out.push_back({Page::Appearance, "", {{"Preview", "", [this](float s, float w) { return preview(s, w); }, true}}});
    out.push_back({Page::Appearance, "Theme", {{"", "", [self](float s, float w) { return self->themeTiles(s, w); }, true}}});
    out.push_back({Page::Appearance,
                   "",
                   {{"Cursor", std::string(kCursorHint),
                     segmented({"Block", "Bar", "Underline"}, static_cast<std::size_t>(now.cursor),
                               [](Settings& x, std::size_t i) { x.cursor = static_cast<CursorShape>(i); })}}});

    // Text
    {
      std::vector<std::string> fonts{"Default (" + std::string(Tokens::fontMono).substr(0, std::string(Tokens::fontMono).find(',')) + ")"};
      std::size_t selected = 0;
      for (const auto& family : m_monoFonts) {
        if (family == now.font) {
          selected = fonts.size();
        }
        fonts.push_back(family);
      }
      if (!now.font.empty() && selected == 0) {
        selected = fonts.size();
        fonts.push_back(now.font); // set by hand to a font fontconfig does not list as mono
      }
      out.push_back({Page::Text,
                     "",
                     {{"Font", "", select(fonts, selected, [fonts](Settings& x, std::size_t i) {
                         x.font = i == 0 ? std::string() : fonts[i];
                       })},
                      {"Size", "",
                       stepper(now.fontSize, SettingsLimits::kFontSizeMin, SettingsLimits::kFontSizeMax, " px",
                               [](Settings& x, int v) { x.fontSize = v; })}}});
    }

    // Terminal
    out.push_back({Page::Terminal,
                   "",
                   {{"Scrollback", "Kept per session; lowering it drops the oldest lines",
                     stepper(now.scrollbackMb, SettingsLimits::kScrollbackMin, SettingsLimits::kScrollbackMax, " MB",
                             [](Settings& x, int v) { x.scrollbackMb = v; })},
                    {"Copy on select", "Selected text goes to the clipboard",
                     toggle(now.copyOnSelect, [](Settings& x, bool on) { x.copyOnSelect = on; })},
                    {"Scroll speed", "Rows per wheel step",
                     stepper(now.scrollRows, SettingsLimits::kScrollRowsMin, SettingsLimits::kScrollRowsMax, " rows",
                             [](Settings& x, int v) { x.scrollRows = v; })}}});

    // Sessions
    {
      std::vector<std::string> shells{"Login shell (" + defaultShell() + ")"};
      std::size_t selected = 0;
      for (const auto& shell : m_shells) {
        if (shell == now.shell) {
          selected = shells.size();
        }
        shells.push_back(shell);
      }
      if (!now.shell.empty() && selected == 0) {
        selected = shells.size();
        shells.push_back(now.shell);
      }
      out.push_back({Page::Sessions,
                     "",
                     {{"Shell", "New sessions", select(shells, selected, [shells](Settings& x, std::size_t i) {
                         x.shell = i == 0 ? std::string() : shells[i];
                       })},
                      {"New sessions start in", "",
                       segmented({"Home", "Current folder"}, static_cast<std::size_t>(now.startDir),
                                 [](Settings& x, std::size_t i) { x.startDir = static_cast<StartDir>(i); })}}});
    }

    // Notifications
    out.push_back({Page::Notifications,
                   "",
                   {{"Notify when a long command finishes", "",
                     toggle(now.notifyLong, [](Settings& x, bool on) { x.notifyLong = on; })},
                    {"Long means at least", "",
                     stepper(now.notifySeconds, SettingsLimits::kNotifyMin, SettingsLimits::kNotifyMax, " s",
                             [](Settings& x, int v) { x.notifySeconds = v; }, now.notifyLong)},
                    {"Only when the window is not focused", "",
                     toggle(now.notifyUnfocusedOnly, [](Settings& x, bool on) { x.notifyUnfocusedOnly = on; },
                            now.notifyLong)}}});

    // Keyboard
    {
      constexpr std::array<std::pair<std::string_view, std::string_view>, 10> kKeys{{
          {"New session", "Ctrl Shift T"},
          {"New window", "Ctrl Shift N"},
          {"New agent session", "Ctrl Shift A"},
          {"Close session", "Ctrl Shift W"},
          {"Next session", "Ctrl Tab"},
          {"Search sessions", "Ctrl K"},
          {"Find", "Ctrl Shift F"},
          {"Copy", "Ctrl Shift C"},
          {"Paste", "Ctrl Shift V"},
          {"Preferences", "Ctrl ,"},
      }};
      Group group{Page::Keyboard, "", {}};
      for (const auto& [label, combo] : kKeys) {
        group.rows.push_back({std::string(label), "", [combo](float s, float) { return keys(std::string(combo), s); }});
      }
      out.push_back(std::move(group));
    }
    return out;
  }

  std::unique_ptr<Flex> PrefsWindow::card(const Group& group, float s, float width) const {
    const float padH = Tokens::spaceP12 * s;
    auto box = ui::column({.align = FlexAlign::Stretch, .width = width});
    box->setCardStyle(s);
    for (std::size_t i = 0; i < group.rows.size(); ++i) {
      const Row& row = group.rows[i];
      if (i > 0) {
        box->addChild(ui::separator({.color = colorSpecFromRole(ColorRole::Separator)}));
      }
      const float inner = width - padH * 2.0F;
      if (row.wide) {
        box->addChild(ui::column({.align = FlexAlign::Start, .paddingV = padH, .paddingH = padH}, row.control(s, inner)));
        continue;
      }
      auto text = ui::column({.align = FlexAlign::Start, .gap = Tokens::spaceP2 * s, .flexGrow = 1.0F});
      text->addChild(ui::label({.text = row.label,
                                .fontSize = Tokens::typeBody.size * s,
                                .color = colorSpecFromRole(ColorRole::Label1)}));
      if (!row.detail.empty()) {
        text->addChild(ui::label({.text = row.detail,
                                  .fontSize = Tokens::typeFootnote.size * s,
                                  .color = colorSpecFromRole(ColorRole::Label2)}));
      }
      box->addChild(ui::row({.align = FlexAlign::Center,
                             .gap = Tokens::spaceP12 * s,
                             .paddingV = Tokens::spaceP8 * s,
                             .paddingH = padH,
                             .minHeight = Tokens::sizeRow * s},
                            std::move(text), row.control(s, inner)));
    }
    return box;
  }

  std::unique_ptr<Node> PrefsWindow::buildPage(float width, float height) {
    const float s = m_window.uiScale();
    const float padH = Tokens::spaceP24 * s;
    const float cardWidth = std::max(0.0F, width - padH * 2.0F);
    auto scroll = ui::scrollView({.state = &m_scroll,
                                  .contentScale = s,
                                  .scrollbarAutoHide = true,
                                  .viewportPaddingH = padH,
                                  .viewportPaddingV = Tokens::spaceP16 * s,
                                  .width = width,
                                  .height = height});
    Flex* content = scroll->content();
    content->setGap(Tokens::spaceP8 * s);
    const auto heading = [&](std::string text) {
      content->addChild(ui::label({.text = std::move(text),
                                   .fontSize = Tokens::typeHeadline.size * s,
                                   .fontWeight = FontWeight::SemiBold,
                                   .color = colorSpecFromRole(ColorRole::Label2)}));
    };

    if (const auto* invalid = std::get_if<SettingsStore::Invalid>(&m_store.fileState())) {
      Group error{m_page, "", {}};
      error.rows.push_back({"The preferences file has an error",
                            "Line " + std::to_string(invalid->error.line) + ": " + invalid->error.message
                                + ". The last good settings stay in use.",
                            [](float sc, float) {
                              return ui::glyph({.glyph = "alert-triangle",
                                                .glyphSize = Tokens::sizeSymbol * sc,
                                                .color = colorSpecFromRole(ColorRole::TermRed)});
                            }});
      content->addChild(card(error, s, cardWidth));
    }

    const std::vector<Group> all = groups();
    if (m_query.empty()) {
      for (const Group& group : all) {
        if (group.page != m_page) {
          continue;
        }
        if (!group.title.empty()) {
          heading(group.title);
        }
        content->addChild(card(group, s, cardWidth));
      }
    } else {
      const std::string q = lower(m_query);
      bool any = false;
      for (std::size_t p = 0; p < kPages.size(); ++p) {
        Group hits{static_cast<Page>(p), "", {}};
        for (const Group& group : all) {
          if (group.page != hits.page) {
            continue;
          }
          const bool titleHit = !group.title.empty() && lower(group.title).contains(q);
          for (const Row& row : group.rows) {
            const std::string name = row.label.empty() ? group.title : row.label;
            if (titleHit || lower(name).contains(q) || lower(row.detail).contains(q)) {
              hits.rows.push_back(row.label.empty() ? Row{group.title, "", row.control, row.wide} : row);
            }
          }
        }
        if (!hits.rows.empty()) {
          heading(std::string(kPages[p].title));
          content->addChild(card(hits, s, cardWidth));
          any = true;
        }
      }
      if (!any) {
        content->addChild(ui::label({.text = "No settings match “" + m_query + "”",
                                     .fontSize = Tokens::typeBody.size * s,
                                     .color = colorSpecFromRole(ColorRole::Label2)}));
      }
    }

    content->addChild(ui::label({.text = "Saved in " + tildePath(m_store.path()) + "; edits there apply at once",
                                 .fontSize = Tokens::typeFootnote.size * s,
                                 .color = colorSpecFromRole(ColorRole::Label3)}));
    return scroll;
  }

  std::unique_ptr<Node> PrefsWindow::preview(float s, float width) const {
    const Settings& now = m_store.settings();
    const Theme theme = now.theme;
    const std::string family =
        now.font.empty() ? std::string(Tokens::fontMono) : now.font + ", " + std::string(Tokens::fontMono);
    const float size = static_cast<float>(now.fontSize) * s;
    auto bg = themeColor(theme, ColorRole::View);
    bg.a = 1.0F;
    auto box = ui::column({.align = FlexAlign::Start,
                           .gap = Tokens::spaceP2 * s,
                           .padding = Tokens::spaceP12 * s,
                           .fill = fixedColorSpec(bg),
                           .radius = Style::scaledRadiusSm(s),
                           .width = width});
    const auto line = [&](std::string text, ColorRole role) {
      box->addChild(ui::label({.text = std::move(text),
                               .fontSize = size,
                               .fontFamily = family,
                               .color = fixedColorSpec(themeColor(theme, role)),
                               .maxWidth = width - Tokens::spaceP12 * s * 2.0F,
                               .maxLines = 1}));
    };
    const char* cursor = now.cursor == CursorShape::Bar ? "▏" : now.cursor == CursorShape::Underline ? "▁" : "█";
    line("~/src/desktop main ❯ cargo test", ColorRole::Label1);
    line("test service::accepts_signed_index ... ok", ColorRole::TermGreen);
    line("test service::resumes_partial ... FAILED", ColorRole::TermRed);
    line(std::string("~/src/desktop main ❯ ") + cursor, ColorRole::Label1);
    return box;
  }

  std::unique_ptr<Node> PrefsWindow::themeTiles(float s, float width) {
    constexpr std::array<std::pair<Theme, std::string_view>, 4> kThemes{
        {{Theme::System, "Andromeda"}, {Theme::Light, "Light"}, {Theme::Dark, "Dark"}, {Theme::Amoled, "AMOLED"}}};
    const float gap = Tokens::spaceP12 * s;
    const float tileW = std::floor((width - gap * 3.0F) / 4.0F);
    const float tileH = std::round(tileW * 0.55F);
    const Theme current = m_store.settings().theme;
    auto row = ui::row({.gap = gap, .width = width});
    for (const auto& [theme, name] : kThemes) {
      const bool on = theme == current;
      auto bg = themeColor(theme, ColorRole::View);
      bg.a = 1.0F;
      auto swatch = ui::column({.align = FlexAlign::Start,
                                .gap = Tokens::spaceP2 * s,
                                .padding = Tokens::spaceP8 * s,
                                .fill = fixedColorSpec(bg),
                                .radius = Style::scaledRadiusSm(s),
                                .border = on ? colorSpecFromRole(ColorRole::Accent) : colorSpecFromRole(ColorRole::Separator),
                                .borderWidth = (on ? 2.0F : 1.0F) * s,
                                .width = tileW,
                                .height = tileH});
      swatch->addChild(ui::label({.text = "~ ❯ ls",
                                  .fontSize = Tokens::typeFootnote.size * s,
                                  .fontFamily = std::string(Tokens::fontMono),
                                  .color = fixedColorSpec(themeColor(theme, ColorRole::Label1))}));
      swatch->addChild(ui::label({.text = "src  docs",
                                  .fontSize = Tokens::typeFootnote.size * s,
                                  .fontFamily = std::string(Tokens::fontMono),
                                  .color = fixedColorSpec(themeColor(theme, ColorRole::TermBlue))}));
      auto tile = ui::column({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s, .width = tileW});
      tile->addChild(std::move(swatch));
      tile->addChild(ui::label({.text = std::string(name),
                                .fontSize = Tokens::typeBody.size * s,
                                .fontWeight = on ? FontWeight::SemiBold : FontWeight::Normal,
                                .color = colorSpecFromRole(ColorRole::Label1)}));
      row->addChild(ui::inputArea({.cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
                                   .width = tileW,
                                   .height = tileH + Tokens::spaceP4 * s + Tokens::typeBody.line * s,
                                   .onClick = [this, theme](const InputArea::PointerData&) {
                                     change([theme](Settings& x) { x.theme = theme; });
                                   }},
                                  std::move(tile)));
    }
    return row;
  }

} // namespace term
