#include "prefs_window.h"

#include "app/app_window.h"
#include "cursor-shape-v1-client-protocol.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "design_tokens.h"
#include "terminal_view.h"
#include "ui/builders.h"
#include "ui/controls/pinned.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_seat.h"

#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>

namespace term {

  namespace {

    constexpr std::string_view kCursorHint = "Programs that choose their own cursor keep it";

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

    struct ThemeChoice {
      std::string id;
      std::string name;
      ThemeSpec spec;
    };

    // The built-in themes, then the valid packages, in tile order.
    std::vector<ThemeChoice> themeChoices(const ThemeStore& store) {
      std::vector<ThemeChoice> out{{"system", "Andromeda", Theme::System},
                                   {"light", "Light", Theme::Light},
                                   {"dark", "Dark", Theme::Dark},
                                   {"amoled", "AMOLED", Theme::Amoled}};
      for (const ThemeEntry& entry : store.entries()) {
        if (const ThemePackage* package = store.find(entry.id)) {
          out.push_back({entry.id, package->name, *package});
        }
      }
      return out;
    }

    std::unique_ptr<Node> keysLabel(std::string text, float s) {
      return ui::label({.text = std::move(text),
                        .fontSize = Tokens::typeBody.size * s,
                        .color = colorSpecFromRole(ColorRole::Label2)});
    }

  } // namespace

  PrefsWindow::PrefsWindow(AppWindow& window, SettingsStore& store, ThemeActions actions)
      : m_window(window), m_store(store), m_actions(std::move(actions)), m_monoFonts(monoFamilies()), m_shells(installedShells()) {
    m_window.setFirstKeyHandler([this](const KeyboardEvent& event) {
      const bool ctrlOnly = (event.modifiers & (KeyMod::Ctrl | KeyMod::Shift | KeyMod::Alt)) == KeyMod::Ctrl;
      if (!event.pressed || !ctrlOnly || (event.unshifted != 'f' && event.unshifted != 'F') || m_search == nullptr) {
        return false;
      }
      m_window.input().setFocus(m_search->inputArea());
      return true;
    });
    m_window.setContentBuilder([this](Renderer&, float width, float height) -> std::unique_ptr<Node> {
      const float s = m_window.uiScale();
      const float gap = Tokens::componentWindowCardGap * s;
      const float columnW = std::round((width / s >= Tokens::componentTerminalPrefsWideFrom ? Tokens::componentTerminalPrefsColumnWide
                                                                                           : Tokens::componentTerminalPrefsColumn)
                                       * s);
      auto body = ui::row({.align = FlexAlign::Start, .width = width, .height = height});
      m_previewWidth = std::max(0.0F, width - columnW - gap);
      m_previewHeight = std::max(0.0F, height - gap);
      auto previewSlot = ui::column({.align = FlexAlign::Stretch, .width = m_previewWidth, .height = m_previewHeight});
      previewSlot->setPadding(0.0F, 0.0F, 0.0F, gap);
      m_previewWidth -= gap;
      m_previewSlot = previewSlot.get();
      previewSlot->addChild(preview(s, m_previewWidth, m_previewHeight));

      const float padH = Tokens::spaceP16 * s;
      auto column = ui::column({.align = FlexAlign::Stretch, .width = columnW, .height = height});
      auto search = ui::searchCapsule({
          .out = &m_search,
          .placeholder = "Search preferences",
          .height = Tokens::sizeControlL,
          .scale = s,
          .value = m_query,
          .onChange = [this](const std::string& text) { setQuery(text); },
      });
      auto searchRow = ui::column({.align = FlexAlign::Stretch, .paddingH = padH}, std::move(search));
      searchRow->setPadding(0.0F, padH, Tokens::spaceP8 * s, padH);
      column->addChild(std::move(searchRow));
      m_slotWidth = columnW;
      m_slotHeight = std::max(0.0F, height - (Tokens::sizeControlL + Tokens::spaceP8) * s);
      auto slot = ui::column({.align = FlexAlign::Stretch, .width = m_slotWidth, .height = m_slotHeight});
      m_slot = slot.get();
      slot->addChild(buildColumn(m_slotWidth, m_slotHeight));
      column->addChild(std::move(slot));

      body->addChild(std::move(previewSlot));
      body->addChild(std::move(column));
      return body;
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

  void PrefsWindow::importTheme(const std::filesystem::path& folder) {
    auto result = importThemeFolder(folder, m_store.themes().dir());
    if (auto* error = std::get_if<SettingsError>(&result)) {
      m_importError = ImportError{folder.filename().string(), std::move(*error)};
      refill();
      return;
    }
    m_importError.reset();
    refill();
    change([id = std::get<std::string>(result)](Settings& x) { x.theme = id; });
  }

  void PrefsWindow::showPane(Pane pane) {
    if (!m_query.empty()) {
      m_query.clear();
      if (m_search != nullptr) {
        m_search->setValue("");
      }
    }
    m_pane = pane;
    m_scroll = {};
    // Called from a control in the column: rebuild after its callback returns, not under it.
    DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
      if (!alive.expired()) {
        refill();
      }
    });
  }

  void PrefsWindow::setQuery(std::string query) {
    m_query = std::move(query);
    m_pane = Pane::Main;
    m_scroll = {};
    refill();
  }

  void PrefsWindow::refill() {
    if (m_slot == nullptr) {
      return;
    }
    m_window.updateInPlace([this](Renderer&) {
      std::vector<std::unique_ptr<Node>> removed;
      for (Flex* slot : {m_slot, m_previewSlot}) {
        while (!slot->children().empty()) {
          removed.push_back(slot->removeChild(slot->children().back().get()));
        }
      }
      m_window.input().forgetDetached(); // while the removed areas still live
      removed.clear();
      m_previewSlot->addChild(preview(m_window.uiScale(), m_previewWidth, m_previewHeight));
      m_previewSlot->markLayoutDirty();
      m_slot->addChild(buildColumn(m_slotWidth, m_slotHeight));
      m_slot->markLayoutDirty();
    });
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

    out.push_back({.pane = Pane::Main,
                   .title = "Theme",
                   .rows = {{"Theme", "", [self](float s, float w) { return self->themeTiles(s, w); }, true}},
                   .bare = true,
                   .themeErrors = true});
    out.push_back({.pane = Pane::Main,
                   .title = "Window",
                   .rows = {{"Window", "", [self](float s, float w) { return self->materialTiles(s, w); }, true}},
                   .bare = true});
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
      out.push_back({Pane::Main,
                     "Text",
                     {{"Font", "", select(fonts, selected, [fonts](Settings& x, std::size_t i) {
                         x.font = i == 0 ? std::string() : fonts[i];
                       })},
                      {"Size", "",
                       stepper(now.fontSize, SettingsLimits::kFontSizeMin, SettingsLimits::kFontSizeMax, " px",
                               [](Settings& x, int v) { x.fontSize = v; })}}});
    }

    out.push_back({Pane::Main,
                   "Cursor",
                   {{"Shape", std::string(kCursorHint),
                     segmented({"Block", "Bar", "Underline"}, static_cast<std::size_t>(now.cursor),
                               [](Settings& x, std::size_t i) { x.cursor = static_cast<CursorShape>(i); })},
                    {"Blink", "", toggle(now.cursorBlink, [](Settings& x, bool on) { x.cursorBlink = on; })},
                    {"Focus outline", "An accent ring around the focused pane",
                     toggle(now.focusRing, [](Settings& x, bool on) { x.focusRing = on; })}}});

    // Terminal
    out.push_back({Pane::Main,
                   "Terminal",
                   {{"Scrollback", "Kept per session; lowering it drops the oldest lines",
                     stepper(now.scrollbackMb, SettingsLimits::kScrollbackMin, SettingsLimits::kScrollbackMax, " MB",
                             [](Settings& x, int v) { x.scrollbackMb = v; })},
                    {"Copy on select", "Selected text goes to the clipboard",
                     toggle(now.copyOnSelect, [](Settings& x, bool on) { x.copyOnSelect = on; })},
                    {"Scroll speed", "Rows per wheel step",
                     stepper(now.scrollRows, SettingsLimits::kScrollRowsMin, SettingsLimits::kScrollRowsMax, " rows",
                             [](Settings& x, int v) { x.scrollRows = v; })},
                    {"Bell", "", segmented({"Flash", "Sound", "Off"}, static_cast<std::size_t>(now.bell),
                                           [](Settings& x, std::size_t i) { x.bell = static_cast<Bell>(i); })}}});

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
      out.push_back({Pane::Main,
                     "Sessions",
                     {{"Shell", "New sessions", select(shells, selected, [shells](Settings& x, std::size_t i) {
                         x.shell = i == 0 ? std::string() : shells[i];
                       })},
                      {"New sessions start in", "",
                       segmented({"Home", "Current folder"}, static_cast<std::size_t>(now.startDir),
                                 [](Settings& x, std::size_t i) { x.startDir = static_cast<StartDir>(i); })}}});
    }

    // Notifications
    out.push_back({Pane::Main,
                   "Notifications",
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
      constexpr std::array<std::pair<std::string_view, std::string_view>, 17> kKeys{{
          {"New session", "Ctrl Shift T"},
          {"New window", "Ctrl Shift N"},
          {"New agent session", "Ctrl Shift A"},
          {"Close session", "Ctrl Shift W"},
          {"Next session", "Ctrl Tab"},
          {"Previous session", "Ctrl PgUp"},
          {"Next session in the list", "Ctrl PgDn"},
          {"Search sessions", "Ctrl Shift K"},
          {"Find", "Ctrl Shift F"},
          {"Copy", "Ctrl Shift C"},
          {"Paste", "Ctrl Shift V"},
          {"Scroll up a page", "Shift PgUp"},
          {"Scroll down a page", "Shift PgDn"},
          {"Larger text", "Ctrl Shift ="},
          {"Smaller text", "Ctrl Shift -"},
          {"Default text size", "Ctrl Shift 0"},
          {"Preferences", "Ctrl ,"},
      }};
      out.push_back({Pane::Main,
                     "Keyboard",
                     {{"Shortcuts", "", [self, n = kKeys.size()](float s, float) -> std::unique_ptr<Node> {
                         const float w = Tokens::sizeControlL * s;
                         const float h = Tokens::sizeControlS * s;
                         auto link = ui::row({.align = FlexAlign::Center,
                                              .justify = FlexJustify::End,
                                              .gap = Tokens::spaceP4 * s,
                                              .width = w,
                                              .height = h});
                         link->addChild(keysLabel(std::to_string(n), s));
                         link->addChild(ui::glyph({.glyph = "chevron-right",
                                                   .glyphSize = Tokens::sizeSymbolInline * s,
                                                   .color = colorSpecFromRole(ColorRole::Label3)}));
                         return ui::inputArea({.cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
                                               .width = w,
                                               .height = h,
                                               .onClick = [self](const InputArea::PointerData&) { self->showPane(Pane::Keyboard); }},
                                              std::move(link));
                       }}}});
      Group group{Pane::Keyboard, "", {}};
      for (const auto& [label, combo] : kKeys) {
        group.rows.push_back({std::string(label), "", [combo](float s, float) -> std::unique_ptr<Node> {
                                auto caps = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s});
                                for (std::string_view rest = combo; !rest.empty();) {
                                  const auto space = rest.find(' ');
                                  caps->addChild(ui::keycap({.text = std::string(rest.substr(0, space)), .scale = s}));
                                  rest = space == std::string_view::npos ? std::string_view() : rest.substr(space + 1);
                                }
                                return caps;
                              }});
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
      const float inner = width - (padH + Style::cardPadding * s) * 2.0F;
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

  std::unique_ptr<Node> PrefsWindow::buildColumn(float width, float height) {
    const float s = m_window.uiScale();
    const float padH = Tokens::spaceP16 * s;
    const float cardWidth = std::max(0.0F, width - padH * 2.0F);
    auto scroll = ui::scrollView({.state = &m_scroll,
                                  .contentScale = s,
                                  .scrollbarAutoHide = true,
                                  .viewportPaddingH = padH,
                                  .viewportPaddingV = Tokens::spaceP8 * s,
                                  .width = width,
                                  .height = height});
    Flex* content = scroll->content();
    content->setGap(Tokens::spaceP8 * s);
    // A section heading: caption 600 label_2 on the card's text inset, 16 more above it than the card gap.
    const auto heading = [&](std::string text, bool first) {
      auto box = ui::column({.align = FlexAlign::Start});
      box->setPadding(first ? 0.0F : Tokens::spaceP16 * s, 0.0F, 0.0F, Tokens::spaceP16 * s);
      box->addChild(ui::label({.text = std::move(text),
                               .fontSize = Tokens::typeFootnote.size * s,
                               .fontWeight = FontWeight::SemiBold,
                               .color = colorSpecFromRole(ColorRole::Label2)}));
      content->addChild(std::move(box));
    };
    // An error: the alert symbol, the title and the detail, and the way out at the end.
    const auto errorCard = [&](std::string title, std::string detail, std::string action, std::function<void()> onAction) {
      const float pad = Tokens::spaceP12 * s;
      auto text = ui::column({.align = FlexAlign::Start, .gap = Tokens::spaceP2 * s, .flexGrow = 1.0F});
      text->addChild(ui::label({.text = std::move(title),
                                .fontSize = Tokens::typeBody.size * s,
                                .color = colorSpecFromRole(ColorRole::Label1)}));
      text->addChild(ui::label({.text = std::move(detail),
                                .fontSize = Tokens::typeFootnote.size * s,
                                .color = colorSpecFromRole(ColorRole::Label2),
                                .maxWidth = cardWidth - Tokens::sizeSymbol * s - pad * 3.0F
                                            - (onAction ? (Tokens::spaceP64 + Tokens::spaceP8) * s : 0.0F)}));
      auto row = ui::row({.align = FlexAlign::Start, .gap = Tokens::spaceP8 * s, .padding = pad, .width = cardWidth},
                         ui::glyph({.glyph = "alert-triangle",
                                    .glyphSize = Tokens::sizeSymbol * s,
                                    .color = colorSpecFromRole(ColorRole::DangerText)}),
                         std::move(text));
      if (onAction) {
        row->addChild(ui::button({.text = std::move(action),
                                  .fontSize = Tokens::typeFootnote.size * s,
                                  .variant = ButtonVariant::Ghost,
                                  .radius = Style::scaledRadiusSm(s),
                                  .height = Tokens::sizeControlS * s,
                                  .onClick = std::move(onAction)}));
      }
      row->setCardStyle(s);
      content->addChild(std::move(row));
    };
    const auto open = [this](std::filesystem::path path) -> std::function<void()> {
      if (!m_actions.openPath) {
        return nullptr;
      }
      return [this, path = std::move(path)] { m_actions.openPath(path); };
    };
    const auto lineOf = [](int line) { return line > 0 ? "Line " + std::to_string(line) + ": " : std::string(); };

    if (const auto* invalid = std::get_if<SettingsStore::Invalid>(&m_store.fileState())) {
      errorCard(m_store.path().filename().string() + " has an error",
                lineOf(invalid->error.line) + invalid->error.message + ". The last good settings stay in use.", "Show file",
                open(m_store.path()));
    }

    const std::vector<Group> all = groups();
    const auto themeErrors = [&] {
      if (m_importError) {
        errorCard("\"" + m_importError->folder + "\" was not imported",
                  lineOf(m_importError->error.line) + m_importError->error.message + ".", "Try again",
                  m_actions.importPackage);
      }
      for (const ThemeEntry& entry : m_store.themes().entries()) {
        if (const auto* invalid = std::get_if<ThemeEntry::Invalid>(&entry.state)) {
          errorCard("Theme \"" + entry.id + "\" has an error",
                    lineOf(invalid->error.line) + invalid->error.message
                        + (invalid->lastGood ? ". The last good version stays in use." : "."),
                    invalid->error.line > 0 ? "Show file" : "Show folder",
                    open(invalid->error.line > 0 ? m_store.themes().dir() / entry.id / "theme.toml" : m_store.themes().dir() / entry.id));
        }
      }
    };
    const auto addGroup = [&](const Group& group, bool first) {
      if (!group.title.empty()) {
        heading(group.title, first);
      }
      if (group.bare) {
        for (const Row& row : group.rows) {
          content->addChild(row.control(s, cardWidth));
        }
      } else {
        content->addChild(card(group, s, cardWidth));
      }
      if (group.themeErrors) {
        themeErrors();
      }
    };

    if (!m_query.empty()) {
      const std::string q = lower(m_query);
      bool any = false;
      for (const Group& group : all) {
        const bool titleHit = !group.title.empty() && lower(group.title).contains(q);
        Group hits{group.pane, group.title, {}, group.bare, false};
        for (const Row& row : group.rows) {
          if (titleHit || lower(row.label).contains(q) || lower(row.detail).contains(q)) {
            hits.rows.push_back(row);
          }
        }
        if (group.pane == Pane::Keyboard && !hits.rows.empty()) {
          hits.title = "Keyboard";
        }
        if (!hits.rows.empty()) {
          addGroup(hits, !any);
          any = true;
        }
      }
      if (!any) {
        auto empty = ui::column({.align = FlexAlign::Center, .paddingV = Tokens::spaceP48 * s, .width = cardWidth});
        empty->addChild(ui::label({.text = "No settings for “" + m_query + "”",
                                   .fontSize = Tokens::typeHeadline.size * s,
                                   .fontWeight = FontWeight::SemiBold,
                                   .color = colorSpecFromRole(ColorRole::Label1)}));
        content->addChild(std::move(empty));
      }
    } else if (m_pane == Pane::Keyboard) {
      auto back = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s, .width = cardWidth, .height = Tokens::sizeRow * s});
      back->addChild(ui::glyph({.glyph = "chevron-left",
                                .glyphSize = Tokens::sizeSymbolInline * s,
                                .color = colorSpecFromRole(ColorRole::AccentText)}));
      back->addChild(ui::label({.text = "Preferences",
                                .fontSize = Tokens::typeBody.size * s,
                                .fontWeight = FontWeight::Medium,
                                .color = colorSpecFromRole(ColorRole::AccentText)}));
      content->addChild(ui::inputArea({.cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
                                       .width = cardWidth,
                                       .height = Tokens::sizeRow * s,
                                       .onClick = [this](const InputArea::PointerData&) { showPane(Pane::Main); }},
                                      std::move(back)));
      auto title = ui::column({.align = FlexAlign::Start, .paddingH = Tokens::spaceP12 * s});
      title->addChild(ui::label({.text = "Keyboard",
                                 .fontSize = Tokens::typeTitle3.size * s,
                                 .fontWeight = FontWeight::SemiBold,
                                 .color = colorSpecFromRole(ColorRole::Label1)}));
      content->addChild(std::move(title));
      for (const Group& group : all) {
        if (group.pane == Pane::Keyboard) {
          content->addChild(card(group, s, cardWidth));
        }
      }
    } else {
      bool first = true;
      for (const Group& group : all) {
        if (group.pane == Pane::Main) {
          addGroup(group, first);
          first = false;
        }
      }
    }
    return scroll;
  }

  std::unique_ptr<Node> PrefsWindow::preview(float s, float width, float height) const {
    const Settings& now = m_store.settings();
    const ThemeSpec theme = resolveTheme(now.theme, m_store.themes());
    const std::string family = gridFont(now, theme);
    const float size = static_cast<float>(now.fontSize) * s;
    const ThemePalette colors = themePalette(theme);
    auto box = ui::column({.align = FlexAlign::Stretch,
                           .fill = fixedColorSpec(colors.view),
                           .radius = Tokens::componentWindowCardRadius * s,
                           .border = colorSpecFromRole(ColorRole::Separator),
                           .borderWidth = Style::borderWidth * s,
                           .width = width,
                           .height = height});
    box->setPadding(Tokens::spaceP16 * s, Tokens::spaceP24 * s);
    const float meta = Tokens::typeFootnote.size * s;
    const float inner = std::max(0.0F, width - Tokens::spaceP24 * s * 2.0F);
    // A line longer than the card is cut at the text edge, as the terminal cuts it.
    const auto line = [&](std::vector<std::pair<std::string, Color>> runs) {
      auto row = ui::row({.gap = 0.0F, .clipChildren = true, .width = inner});
      for (auto& [text, color] : runs) {
        row->addChild(ui::label({.text = std::move(text),
                                 .fontSize = size,
                                 .fontFamily = family,
                                 .color = fixedColorSpec(color),
                                 .maxLines = 1}));
      }
      box->addChild(std::move(row));
    };
    // The prompt's context line: folder and branch pills, the last command's result at the end.
    const auto context = [&](std::string result, bool first) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s});
      if (!first) {
        row->setPadding(size * 1.4F, 0.0F, 0.0F, 0.0F);
      }
      const auto pill = [&](std::string glyph, std::string text, Color fg, Color bg) {
        auto p = ui::row({.align = FlexAlign::Center,
                          .gap = Tokens::spaceP4 * s,
                          .paddingH = Tokens::spaceP8 * s,
                          .fill = fixedColorSpec(bg),
                          .radius = Tokens::typeFootnote.line * s / 2.0F,
                          .minHeight = Tokens::typeFootnote.line * s});
        p->addChild(ui::glyph({.glyph = std::move(glyph), .glyphSize = Tokens::sizeSymbolInline * s, .color = fixedColorSpec(fg)}));
        p->addChild(ui::label({.text = std::move(text), .fontSize = meta, .fontWeight = FontWeight::Medium, .color = fixedColorSpec(fg)}));
        row->addChild(std::move(p));
      };
      pill("folder", "~/src/desktop", colors.ansi[4], lerpColor(colors.view, colors.ansi[4], 0.14F));
      pill("git-branch", "main", colors.label2, lerpColor(colors.view, colors.label1, 0.10F));
      row->addChild(ui::column({.flexGrow = 1.0F}));
      if (!result.empty()) {
        row->addChild(ui::label({.text = std::move(result), .fontSize = meta, .color = fixedColorSpec(colors.label2)}));
      }
      box->addChild(std::move(row));
    };
    const char* cursor = now.cursor == CursorShape::Bar ? "▏" : now.cursor == CursorShape::Underline ? "▁" : "█";
    context("2.4 s", true);
    line({{"λ  ", colors.accent}, {"cargo test -p service", colors.label1}});
    line({{"   Compiling service v0.4.1 (/src/desktop/service)", colors.label2}});
    line({{"test service::accepts_signed_index ... ", colors.label1}, {"ok", colors.ansi[2]}});
    line({{"warning", colors.ansi[3]}, {": unused import ", colors.label1}, {"`std::io`", colors.ansi[6]}});
    line({{"test service::resumes_partial ... ", colors.label1}, {"FAILED", colors.ansi[1]}});
    context("exit 101 · 0.8 s", false);
    line({{"λ  ", colors.accent}, {"git log --oneline -2", colors.label1}});
    line({{"e1e5790", colors.ansi[3]}, {" Terminal themes show their own colours", colors.label1}});
    line({{"4873f2d", colors.ansi[3]}, {" Terminal theme Import…", colors.label1}});
    context("", false);
    line({{"λ  ", colors.accent}, {cursor, colors.accent}});

    box->addChild(ui::column({.flexGrow = 1.0F}));
    std::string name = now.theme;
    for (const ThemeChoice& choice : themeChoices(m_store.themes())) {
      if (choice.id == now.theme) {
        name = choice.name;
      }
    }
    const std::string fontName = now.font.empty() ? std::string(Tokens::fontMono).substr(0, std::string(Tokens::fontMono).find(',')) : now.font;
    auto status = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    const float dot = Tokens::spaceP8 * s;
    status->addChild(ui::column({.fill = fixedColorSpec(colors.accent), .radius = dot / 2.0F, .width = dot, .height = dot}));
    status->addChild(ui::label({.text = name, .fontSize = meta, .fontWeight = FontWeight::SemiBold, .color = fixedColorSpec(colors.label1)}));
    status->addChild(ui::label({.text = "·  " + fontName + " " + std::to_string(now.fontSize) + "  ·  "
                                    + (now.material == WindowMaterial::Glass ? "Glass" : "Solid"),
                                .fontSize = meta,
                                .fontWeight = FontWeight::Medium,
                                .color = fixedColorSpec(colors.label2),
                                .maxLines = 1,
                                .ellipsize = TextEllipsize::End,
                                .flexGrow = 1.0F}));
    box->addChild(std::move(status));
    return box;
  }

  std::unique_ptr<Node> PrefsWindow::checked(std::unique_ptr<Node> swatch, bool on, float s) {
    if (!on) {
      return swatch;
    }
    const float d = Tokens::sizeSymbolL * s;
    auto pinned = std::make_unique<Pinned>(Tokens::spaceP4 * s);
    pinned->addChild(std::move(swatch));
    pinned->addChild(ui::column({.align = FlexAlign::Center,
                                 .justify = FlexJustify::Center,
                                 .fill = colorSpecFromRole(ColorRole::Accent),
                                 .radius = d / 2.0F,
                                 .width = d,
                                 .height = d},
                                ui::glyph({.glyph = std::string("check"),
                                           .glyphSize = Tokens::sizeSymbolInline * s,
                                           .color = colorSpecFromRole(ColorRole::OnAccent)})));
    return pinned;
  }

  std::unique_ptr<Node> PrefsWindow::tile(std::unique_ptr<Flex> swatch, std::string_view name, bool on, float s,
                                          float tileW, float tileH,
                                          std::function<void(const InputArea::PointerData&)> pick) {
    swatch->setBorder(colorSpecFromRole(on ? ColorRole::Label1 : ColorRole::Separator),
                      (on ? Style::focusRingWidth : Style::borderWidth) * s);
    auto column = ui::column({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .width = tileW});
    column->addChild(checked(std::move(swatch), on, s));
    column->addChild(ui::label({.text = std::string(name),
                                .fontSize = Tokens::typeFootnote.size * s,
                                .fontWeight = on ? FontWeight::SemiBold : FontWeight::Normal,
                                .color = colorSpecFromRole(on ? ColorRole::Label1 : ColorRole::Label2),
                                .maxWidth = tileW,
                                .maxLines = 1}));
    return ui::inputArea({.cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
                          .width = tileW,
                          .height = tileH + Tokens::spaceP8 * s + Tokens::typeFootnote.line * s,
                          .onClick = [pick = std::move(pick)](const InputArea::PointerData& data) { pick(data); }},
                         std::move(column));
  }

  std::unique_ptr<Node> PrefsWindow::materialTiles(float s, float width) {
    constexpr std::array<std::pair<WindowMaterial, std::string_view>, 2> kMaterials{
        {{WindowMaterial::Solid, "Solid"}, {WindowMaterial::Glass, "Glass"}}};
    const float gap = Tokens::spaceP12 * s;
    const float tileW = std::floor((width - gap) / 2.0F);
    const float tileH = std::round(tileW * Tokens::componentTerminalWindowTileAspect);
    const float inset = Tokens::spaceP12 * s;
    const WindowMaterial current = m_store.settings().material;
    auto row = ui::row({.gap = gap, .width = width});
    for (const auto& [material, name] : kMaterials) {
      const bool on = material == current;
      const bool isGlass = material == WindowMaterial::Glass;
      const float innerW = tileW - inset * 2.0F;
      const float sideW = std::round(innerW * Tokens::componentTerminalWindowTileSidebar);
      const float r = Style::scaledRadiusSm(s);
      auto window = ui::row({.width = innerW, .height = tileH - inset * 2.0F});
      const float tint = isGlass ? Style::glassMaterial(glass::Kind::Sheet).tint : 1.0F;
      auto side = ui::column({.fill = colorSpecFromRole(ColorRole::SolidThin, tint),
                              .fillHeight = true,
                              .width = sideW});
      side->setRadii(Radii{r, 0.0F, 0.0F, r});
      auto main = ui::column({.fill = colorSpecFromRole(ColorRole::SolidRegular, tint),
                              .fillHeight = true,
                              .flexGrow = 1.0F});
      main->setRadii(Radii{0.0F, r, r, 0.0F});
      window->addChild(std::move(side));
      window->addChild(std::move(main));
      auto swatch = ui::column({.align = FlexAlign::Start,
                                .padding = inset,
                                .fill = colorSpecFromRole(ColorRole::Accent),
                                .radius = Style::scaledRadiusSm(s),
                                .width = tileW,
                                .height = tileH},
                               std::move(window));
      row->addChild(tile(std::move(swatch), name, on, s, tileW, tileH,
                         [this, material](const InputArea::PointerData&) {
                           change([material](Settings& x) { x.material = material; });
                         }));
    }
    return row;
  }

  std::unique_ptr<Node> PrefsWindow::themeTiles(float s, float width) {
    const std::vector<ThemeChoice> choices = themeChoices(m_store.themes());
    const std::size_t kPerRow = width / s >= Tokens::componentTerminalPrefsColumnWide - Tokens::spaceP32 ? 4 : 3;
    const float gap = Tokens::spaceP8 * s;
    const float tileW = std::floor((width - gap * static_cast<float>(kPerRow - 1)) / static_cast<float>(kPerRow));
    const float tileH = std::round(tileW * Tokens::componentTerminalThemeTileAspect);
    const std::string& current = m_store.settings().theme;
    auto grid = ui::column({.align = FlexAlign::Start, .gap = Tokens::spaceP12 * s, .width = width});
    std::unique_ptr<Flex> row;
    for (std::size_t i = 0; i < choices.size(); ++i) {
      if (i % kPerRow == 0) {
        if (row) {
          grid->addChild(std::move(row));
        }
        row = ui::row({.gap = gap, .width = width});
      }
      const ThemeChoice& choice = choices[i];
      const ThemePalette colors = themePalette(choice.spec);
      auto swatch = ui::column({.align = FlexAlign::Start,
                                .gap = Tokens::spaceP2 * s,
                                .padding = Tokens::spaceP8 * s,
                                .fill = fixedColorSpec(colors.view),
                                .radius = Style::scaledRadiusSm(s),
                                .width = tileW,
                                .height = tileH});
      auto prompt = ui::row({.gap = 0.0F});
      for (const auto& [text, color] : {std::pair{"❯ ", colors.accent}, std::pair{"ls", colors.label1}}) {
        prompt->addChild(ui::label({.text = text,
                                    .fontSize = Tokens::typeFootnote.size * s,
                                    .fontFamily = std::string(Tokens::fontMono),
                                    .color = fixedColorSpec(color)}));
      }
      swatch->addChild(std::move(prompt));
      auto range = ui::row({.gap = Tokens::spaceP2 * s});
      for (int i = 1; i <= 6; ++i) {
        range->addChild(ui::column({.fill = fixedColorSpec(colors.ansi[i]),
                                    .radius = Tokens::spaceP4 * s,
                                    .width = Tokens::spaceP8 * s,
                                    .height = Tokens::spaceP8 * s}));
      }
      swatch->addChild(std::move(range));
      row->addChild(tile(std::move(swatch), choice.name, choice.id == current, s, tileW, tileH,
                         [this, id = choice.id](const InputArea::PointerData&) {
                           change([id](Settings& x) { x.theme = id; });
                         }));
    }
    if (choices.size() % kPerRow == 0) {
      grid->addChild(std::move(row));
      row = ui::row({.gap = gap, .width = width});
    }
    auto add = ui::column({.align = FlexAlign::Center,
                           .justify = FlexJustify::Center,
                           .fill = colorSpecFromRole(ColorRole::Fill1),
                           .radius = Style::scaledRadiusSm(s),
                           .width = tileW,
                           .height = tileH},
                          ui::glyph({.glyph = std::string("plus"),
                                     .glyphSize = Tokens::sizeSymbolInline * s,
                                     .color = colorSpecFromRole(ColorRole::Label2)}));
    row->addChild(tile(std::move(add), "Add theme", false, s, tileW, tileH, [this](const InputArea::PointerData& at) {
      enum : std::int32_t { kCreate = 1, kImport, kFolder };
      m_window.openContextMenu({{.id = kCreate, .label = "Create with Claude", .glyph = "robot"},
                                {.id = kImport, .label = "Import…", .glyph = "file-import"},
                                {.id = kFolder, .label = "Open themes folder", .glyph = "folder"}},
                               at.sceneX, at.sceneY, [this](std::int32_t item) {
                                 const auto& action = item == kCreate   ? m_actions.createWithAgent
                                                      : item == kImport ? m_actions.importPackage
                                                                        : m_actions.openFolder;
                                 if (action) {
                                   action();
                                 }
                               });
    }));
    grid->addChild(std::move(row));
    return grid;
  }

} // namespace term
