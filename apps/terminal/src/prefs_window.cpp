#include "prefs_window.h"

#include "i18n/i18n.h"
#include "platform/input_events.h"
#include "app/app_window.h"
#include "core/input/cursor_shape.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "design_tokens.h"
#include "terminal_view.h"
#include "ui/builders.h"
#include "core/files/resource_paths.h"
#include "ui/controls/image.h"
#include "ui/controls/pinned.h"
#include "ui/palette.h"
#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"
#include "render/scene/shader_scene_node.h"
#include "ui/style.h"

#include <fontconfig/fontconfig.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <cmath>
#include <map>
#include <set>

namespace term {

  namespace {
    std::string_view materialName(WindowMaterial material) {
    switch (material) {
    case WindowMaterial::Solid: return i18n::tr("terminal.prefs.material-solid");
    case WindowMaterial::Glass: return i18n::tr("terminal.prefs.material-glass");
    case WindowMaterial::GlassSidebar: return i18n::tr("terminal.prefs.material-glass-sidebar");
    case WindowMaterial::GlassContent: return i18n::tr("terminal.prefs.material-glass-content");
    }
    return i18n::tr("terminal.prefs.material-solid");
  }

  } // namespace

  // A clipped slot whose children sit at their layout place plus a slide offset; a child out of the layout keeps the
  // place it had in it. An outgoing page stays in the slot out of the layout until its spring ends.
  class SlideSlot final : public Flex {
  public:
    void setSlide(Node* child, float x, float y) {
      m_slide[child] = {x, y};
      place();
      markPaintDirty();
    }
    void forget(Node* child) {
      m_slide.erase(child);
      m_base.erase(child);
    }

  protected:
    void doLayout(Renderer& renderer) override {
      Flex::doLayout(renderer);
      takeBase();
    }
    void doArrange(Renderer& renderer, const LayoutRect& rect) override {
      Flex::doArrange(renderer, rect);
      takeBase();
    }

  private:
    void takeBase() {
      for (const auto& child : children()) {
        if (child->participatesInLayout()) {
          m_base[child.get()] = {child->x(), child->y()};
        }
      }
      place();
    }
    void place() {
      for (const auto& [child, slide] : m_slide) {
        const auto base = m_base.find(child);
        const auto [bx, by] = base != m_base.end() ? base->second : std::pair{0.0F, 0.0F};
        child->setPosition(bx + slide.first, by + slide.second);
      }
    }
    std::map<Node*, std::pair<float, float>> m_slide;
    std::map<Node*, std::pair<float, float>> m_base;
  };

  namespace {

    std::unique_ptr<SlideSlot> slideSlot(float width, float height) {
      auto slot = std::make_unique<SlideSlot>();
      slot->setDirection(FlexDirection::Vertical);
      slot->setAlign(FlexAlign::Stretch);
      slot->setClipChildren(true);
      slot->setMinWidth(width);
      slot->setMaxWidth(width);
      slot->setMinHeight(height);
      slot->setMaxHeight(height);
      return slot;
    }

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

    std::unique_ptr<Node> keysLabel(std::string text, float s) {
      return ui::label({.text = std::move(text),
                        .fontSize = Tokens::typeBody.size * s,
                        .color = colorSpecFromRole(ColorRole::Label2)});
    }

  } // namespace

  PrefsWindow::PrefsWindow(AppWindow& window, SettingsStore& store, ThemeActions actions)
      : m_window(window), m_store(store), m_actions(std::move(actions)), m_monoFonts(monoFamilies()), m_shells(installedShells()) {
    m_window.setFirstKeyHandler([this](const KeyboardEvent& event) {
      if (captureKey(event)) {
        return true;
      }
      const bool ctrlOnly = (event.modifiers & (KeyMod::Ctrl | KeyMod::Shift | KeyMod::Alt)) == KeyMod::Ctrl;
      if (!event.pressed || !ctrlOnly || (event.unshifted != 'f' && event.unshifted != 'F') || m_search == nullptr) {
        return false;
      }
      m_window.input().setFocus(m_search->inputArea());
      return true;
    });
    m_window.setContentBuilder([this](Renderer& renderer, float width, float height) -> std::unique_ptr<Node> {
      const float s = m_window.uiScale();
      const float gap = Tokens::componentWindowCardGap * s;
      const float columnW = std::round((width / s >= Tokens::componentTerminalPrefsWideFrom ? Tokens::componentTerminalPrefsColumnWide
                                                                                           : Tokens::componentTerminalPrefsColumn)
                                       * s);
      auto body = ui::row({.align = FlexAlign::Start, .width = width, .height = height});
      m_previewWidth = std::max(0.0F, width - columnW - gap);
      m_previewHeight = std::max(0.0F, height - gap);
      auto previewSlot = slideSlot(m_previewWidth, m_previewHeight);
      previewSlot->setPadding(0.0F, 0.0F, 0.0F, gap);
      m_previewWidth -= gap;
      m_previewSlot = previewSlot.get();
      m_previewTheme = m_store.settings().theme;
      previewSlot->addChild(preview(renderer, s, m_previewWidth, m_previewHeight));

      const float padH = Tokens::spaceP16 * s;
      auto column = ui::column({.align = FlexAlign::Stretch, .width = columnW, .height = height});
      auto search = ui::searchCapsule({
          .out = &m_search,
          .placeholder = i18n::tr("terminal.prefs.search"),
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
      auto slot = slideSlot(m_slotWidth, m_slotHeight);
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

  void PrefsWindow::setCapture(Capture next) {
    m_capture = std::move(next);
    // Called from a row's click: rebuild after its callback returns, not under it.
    DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
      if (!alive.expired()) {
        refill();
      }
    });
  }

  bool PrefsWindow::captureKey(const KeyboardEvent& event) {
    if (std::holds_alternative<Idle>(m_capture)) {
      return false;
    }
    if (!event.pressed || event.repeat) {
      return true;
    }
    const auto* replacing = std::get_if<Replacing>(&m_capture);
    const Action action = replacing != nullptr ? replacing->action : std::get<Recording>(m_capture).action;
    const std::uint32_t mods = event.modifiers & (KeyMod::Ctrl | KeyMod::Shift | KeyMod::Alt | KeyMod::Super);
    if (mods == 0 && event.sym == XKB_KEY_Escape) {
      setCapture(Idle{});
      return true;
    }
    if (mods == 0 && event.sym == XKB_KEY_BackSpace) {
      setCapture(Idle{});
      change([action](Settings& x) { x.keys.bind(action, {}); });
      return true;
    }
    const bool modifierKey = (event.sym >= XKB_KEY_Shift_L && event.sym <= XKB_KEY_Hyper_R) || event.sym == XKB_KEY_ISO_Level3_Shift
                             || event.sym == XKB_KEY_ISO_Level5_Shift;
    const bool functionKey = event.sym >= XKB_KEY_F1 && event.sym <= XKB_KEY_F24;
    const bool pageKey = mods == KeyMod::Shift
                         && (event.sym == XKB_KEY_Page_Up || event.sym == XKB_KEY_Page_Down || event.sym == XKB_KEY_Home
                             || event.sym == XKB_KEY_End);
    // A plain or Shift-only key belongs to the program in the terminal.
    if (modifierKey || ((mods & (KeyMod::Ctrl | KeyMod::Alt)) == 0 && !functionKey && !pageKey)) {
      return true;
    }
    const Chord chord = chordOf(event.sym, event.unshifted, event.modifiers);
    const std::optional<Action> owner = m_store.settings().keys.actionOf(chord);
    if (owner && *owner != action && (replacing == nullptr || replacing->chord != chord)) {
      setCapture(Replacing{action, chord, *owner});
      return true;
    }
    setCapture(Idle{});
    change([action, chord](Settings& x) { x.keys.bind(action, {chord}); });
    return true;
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
    m_capture = Idle{};
    m_scroll = {};
    // Called from a control in the column: rebuild after its callback returns, not under it.
    DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
      if (!alive.expired()) {
        refill(m_pane == Pane::Keyboard ? Transition::Forward : Transition::Back);
      }
    });
  }

  void PrefsWindow::setQuery(std::string query) {
    m_query = std::move(query);
    m_pane = Pane::Main;
    m_scroll = {};
    refill(Transition::Fade);
  }

  void PrefsWindow::refill(Transition transition) {
    if (m_slot == nullptr) {
      return;
    }
    m_window.updateInPlace([this, transition](Renderer& renderer) {
      const bool reduced = MotionService::instance().reduced();
      const bool themeChanged = m_store.settings().theme != m_previewTheme;
      m_previewTheme = m_store.settings().theme;
      // Takes every child out of the slot but the current one when it is to animate out.
      std::vector<std::unique_ptr<Node>> removed;
      const auto clear = [&](SlideSlot* slot, bool keepLast) {
        std::unique_ptr<Node> kept;
        while (!slot->children().empty()) {
          Node* child = slot->children().back().get();
          slot->forget(child);
          auto node = slot->removeChild(child);
          if (keepLast && !kept && node->participatesInLayout()) {
            kept = std::move(node);
          } else {
            removed.push_back(std::move(node));
          }
        }
        return kept;
      };
      auto oldPreview = clear(m_previewSlot, themeChanged);
      auto oldColumn = clear(m_slot, transition == Transition::Forward || transition == Transition::Back);
      m_window.input().forgetDetached(); // while the removed areas still live
      removed.clear();

      // Removes an outgoing node when its spring ends; the node's end cancels the spring first.
      const auto retire = [this](SlideSlot* slot, Node* node) {
        return [this, slot, node] {
          slot->forget(node);
          auto gone = slot->removeChild(node);
          m_window.input().forgetDetached();
          m_window.invalidate();
        };
      };
      AnimationManager& animations = m_window.animations();

      m_previewSlot->addChild(preview(renderer, m_window.uiScale(), m_previewWidth, m_previewHeight));
      m_previewSlot->markLayoutDirty();
      if (oldPreview) {
        // P14-style crossfade: the old theme's preview fades out over the new one.
        Node* old = oldPreview.get();
        old->setParticipatesInLayout(false);
        m_previewSlot->addChild(std::move(oldPreview));
        animations.drive(0, 1.0F, 0.0F, Style::spring(Tokens::springFade), [old](float v) { old->setOpacity(v); },
                         retire(m_previewSlot, old), old);
      }

      auto fresh = buildColumn(m_slotWidth, m_slotHeight);
      Node* now = fresh.get();
      m_slot->addChild(std::move(fresh));
      m_slot->markLayoutDirty();
      if (transition == Transition::Fade || (oldColumn && reduced)) {
        // P13 content fade (and the reduced-motion form of a page change): the new content rises from 35 %.
        oldColumn.reset();
        m_window.input().forgetDetached();
        now->setOpacity(0.35F);
        animations.drive(0, 0.35F, 1.0F, Style::spring(Tokens::springFade), [now](float v) { now->setOpacity(v); }, {}, now);
      } else if (oldColumn) {
        // P12 page slide: the deeper page comes in from the trailing edge with a 32 gap; the shallower page moves 30 %
        // and dims 10 %. Back reverses it, with the outgoing deeper page on top.
        const bool forward = transition == Transition::Forward;
        const float travel = m_slotWidth + Tokens::spaceP32 * m_window.uiScale();
        const float parallax = m_slotWidth * 0.3F;
        Node* old = oldColumn.get();
        old->setParticipatesInLayout(false);
        if (forward) {
          m_slot->insertChildAt(0, std::move(oldColumn));
        } else {
          m_slot->addChild(std::move(oldColumn));
        }
        SlideSlot* slot = m_slot;
        const auto step = [slot, now, old, forward, travel, parallax](float t) {
          const float deep = forward ? 1.0F - t : t;    // the Keyboard page's way in: 0 = in place, 1 = off
          const float shallow = forward ? t : 1.0F - t; // the main page's way out: 0 = in place, 1 = 30 % back
          Node* keyboard = forward ? now : old;
          Node* main = forward ? old : now;
          slot->setSlide(keyboard, travel * deep, 0.0F);
          slot->setSlide(main, -parallax * shallow, 0.0F);
          main->setOpacity(1.0F - 0.1F * shallow);
        };
        step(0.0F);
        animations.drive(0, 0.0F, 1.0F, Style::spring(Tokens::springPage),
                         [this, step](float t) {
                           step(t);
                           m_window.invalidate();
                         },
                         [slot, old, now, retire] {
                           slot->forget(now);
                           now->setOpacity(1.0F);
                           retire(slot, old)();
                         },
                         old);
      }
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
                   .title = i18n::tr("terminal.prefs.theme"),
                   .rows = {{i18n::tr("terminal.prefs.theme"), "", [self](float s, float w) { return self->themeTiles(s, w); }, true}},
                   .bare = true,
                   .themeErrors = true});
    out.push_back({.pane = Pane::Main,
                   .title = i18n::tr("terminal.prefs.window"),
                   .rows = {{i18n::tr("terminal.prefs.window"), "", [self](float s, float w) { return self->materialTiles(s, w); }, true}},
                   .bare = true});
    // Text
    {
      std::vector<std::string> fonts{i18n::tr("terminal.prefs.font-default", "name", std::string(Tokens::fontMono).substr(0, std::string(Tokens::fontMono).find(',')))};
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
                     i18n::tr("terminal.prefs.text"),
                     {{i18n::tr("terminal.prefs.font"), "", select(fonts, selected, [fonts](Settings& x, std::size_t i) {
                         x.font = i == 0 ? std::string() : fonts[i];
                       })},
                      {i18n::tr("terminal.prefs.size"), "",
                       stepper(now.fontSize, SettingsLimits::kFontSizeMin, SettingsLimits::kFontSizeMax, " px",
                               [](Settings& x, int v) { x.fontSize = v; })},
                      {i18n::tr("terminal.prefs.bright-bold"), i18n::tr("terminal.prefs.bright-bold-hint"),
                       toggle(now.boldIsBright, [](Settings& x, bool on) { x.boldIsBright = on; })},
                      {i18n::tr("terminal.prefs.padding"), i18n::tr("terminal.prefs.padding-hint"),
                       stepper(now.padding, SettingsLimits::kPaddingMin, SettingsLimits::kPaddingMax, " px",
                               [](Settings& x, int v) { x.padding = v; })}}});
    }

    out.push_back({Pane::Main,
                   i18n::tr("terminal.prefs.cursor"),
                   {{i18n::tr("terminal.prefs.cursor-shape"), i18n::tr("terminal.prefs.cursor-hint"),
                     segmented({i18n::tr("terminal.prefs.cursor-block"), i18n::tr("terminal.prefs.cursor-bar"), i18n::tr("terminal.prefs.cursor-underline")}, static_cast<std::size_t>(now.cursor),
                               [](Settings& x, std::size_t i) { x.cursor = static_cast<CursorShape>(i); })},
                    {i18n::tr("terminal.prefs.blink"), "", toggle(now.cursorBlink, [](Settings& x, bool on) { x.cursorBlink = on; })},
                    {i18n::tr("terminal.prefs.focus-outline"), i18n::tr("terminal.prefs.focus-outline-hint"),
                     toggle(now.focusRing, [](Settings& x, bool on) { x.focusRing = on; })}}});

    // Terminal
    out.push_back({Pane::Main,
                   i18n::tr("terminal.prefs.terminal"),
                   {{i18n::tr("terminal.prefs.scrollback"), i18n::tr("terminal.prefs.scrollback-hint"),
                     stepper(now.scrollbackMb, SettingsLimits::kScrollbackMin, SettingsLimits::kScrollbackMax, " MB",
                             [](Settings& x, int v) { x.scrollbackMb = v; })},
                    {i18n::tr("terminal.prefs.unlimited-scrollback"), i18n::tr("terminal.prefs.unlimited-scrollback-hint"),
                     toggle(now.scrollback == Scrollback::Unlimited,
                            [](Settings& x, bool on) { x.scrollback = on ? Scrollback::Unlimited : Scrollback::Limited; })},
                    {i18n::tr("terminal.prefs.copy-on-select"), i18n::tr("terminal.prefs.copy-on-select-hint"),
                     toggle(now.copyOnSelect, [](Settings& x, bool on) { x.copyOnSelect = on; })},
                    {i18n::tr("terminal.prefs.focus-follows-mouse"), i18n::tr("terminal.prefs.focus-follows-mouse-hint"),
                     toggle(now.focusFollowsMouse, [](Settings& x, bool on) { x.focusFollowsMouse = on; })},
                    {i18n::tr("terminal.prefs.scroll-speed"), i18n::tr("terminal.prefs.scroll-speed-hint"),
                     stepper(now.scrollRows, SettingsLimits::kScrollRowsMin, SettingsLimits::kScrollRowsMax, " rows",
                             [](Settings& x, int v) { x.scrollRows = v; })},
                    {i18n::tr("terminal.prefs.bell"), "", segmented({i18n::tr("terminal.prefs.bell-flash"), i18n::tr("terminal.prefs.bell-sound"), i18n::tr("terminal.prefs.bell-off")}, static_cast<std::size_t>(now.bell),
                                           [](Settings& x, std::size_t i) { x.bell = static_cast<Bell>(i); })}}});

    // Sessions
    {
      std::vector<std::string> shells{i18n::tr("terminal.prefs.login-shell", "name", defaultShell())};
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
                     i18n::tr("terminal.prefs.sessions"),
                     {{i18n::tr("terminal.prefs.shell"), i18n::tr("terminal.prefs.shell-hint"), select(shells, selected, [shells](Settings& x, std::size_t i) {
                         x.shell = i == 0 ? std::string() : shells[i];
                       })},
                      {i18n::tr("terminal.prefs.start-dir"), "",
                       segmented({i18n::tr("terminal.prefs.start-home"), i18n::tr("terminal.prefs.start-current")}, static_cast<std::size_t>(now.startDir),
                                 [](Settings& x, std::size_t i) { x.startDir = static_cast<StartDir>(i); })},
                      {i18n::tr("terminal.prefs.restore-sessions"), i18n::tr("terminal.prefs.restore-sessions-hint"),
                       toggle(now.restoreSessions, [](Settings& x, bool on) { x.restoreSessions = on; })}}});
    }

    // Notifications
    out.push_back({Pane::Main,
                   i18n::tr("terminal.prefs.notifications"),
                   {{i18n::tr("terminal.prefs.notify-long"), "",
                     toggle(now.notifyLong, [](Settings& x, bool on) { x.notifyLong = on; })},
                    {i18n::tr("terminal.prefs.notify-seconds"), "",
                     stepper(now.notifySeconds, SettingsLimits::kNotifyMin, SettingsLimits::kNotifyMax, " s",
                             [](Settings& x, int v) { x.notifySeconds = v; }, now.notifyLong)},
                    {i18n::tr("terminal.prefs.notify-unfocused"), "",
                     toggle(now.notifyUnfocusedOnly, [](Settings& x, bool on) { x.notifyUnfocusedOnly = on; },
                            now.notifyLong)}}});

    // Keyboard
    {
      const std::size_t bound = std::ranges::count_if(actionTable(), [&](const ActionInfo& info) { return !now.keys.chords(info.action).empty(); });
      out.push_back({Pane::Main,
                     i18n::tr("terminal.prefs.keyboard"),
                     {{i18n::tr("terminal.prefs.shortcuts"), "", [self, n = bound](float s, float) -> std::unique_ptr<Node> {
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
                         return ui::inputArea({.cursorShape = ::CursorShape::Pointer,
                                               .width = w,
                                               .height = h,
                                               .onClick = [self](const InputArea::PointerData&) { self->showPane(Pane::Keyboard); }},
                                              std::move(link));
                       }}}});
      const auto labelOf = [](Action action) {
        const auto table = actionTable();
        const auto it = std::ranges::find(table, action, &ActionInfo::action);
        return it == table.end() ? std::string() : std::string(it->label);
      };
      Group group{Pane::Keyboard, "", {}};
      for (const ActionInfo& info : actionTable()) {
        const Action action = info.action;
        std::string combo = now.keys.label(action, " ");
        std::string prompt;
        if (const auto* r = std::get_if<Recording>(&m_capture); r != nullptr && r->action == action) {
          prompt = i18n::tr("terminal.prefs.type-shortcut");
        } else if (const auto* q = std::get_if<Replacing>(&m_capture); q != nullptr && q->action == action) {
          prompt = i18n::tr("terminal.prefs.used-by", "name", labelOf(q->other));
        }
        const bool active = !prompt.empty();
        if (!active && combo.empty()) {
          prompt = i18n::tr("terminal.prefs.none");
        }
        group.rows.push_back(
            {.label = std::string(info.label),
             .control = [combo = std::move(combo), prompt = std::move(prompt), active](float s, float w) -> std::unique_ptr<Node> {
               if (!prompt.empty()) {
                 return ui::label({.text = prompt,
                                   .fontSize = Tokens::typeBody.size * s,
                                   .color = colorSpecFromRole(active ? ColorRole::AccentText : ColorRole::Label2),
                                   .maxWidth = w * 0.6F,
                                   .maxLines = 1,
                                   .ellipsize = TextEllipsize::End});
               }
               auto caps = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s});
               for (std::string_view rest = combo; !rest.empty();) {
                 const auto space = rest.find(' ');
                 caps->addChild(ui::keycap({.text = std::string(rest.substr(0, space)), .scale = s}));
                 rest = space == std::string_view::npos ? std::string_view() : rest.substr(space + 1);
               }
               return caps;
             },
             .onClick = [self, action] { self->setCapture(Recording{action}); }});
      }
      out.push_back(std::move(group));
      if (now.keys != Keybindings{}) {
        out.push_back({Pane::Keyboard,
                       "",
                       {{i18n::tr("terminal.prefs.restore-shortcuts"), "", [self](float s, float) -> std::unique_ptr<Node> {
                           return ui::button({.text = i18n::tr("terminal.prefs.restore"),
                                              .fontSize = Tokens::typeFootnote.size * s,
                                              .variant = ButtonVariant::Ghost,
                                              .radius = Style::scaledRadiusSm(s),
                                              .height = Tokens::sizeControlS * s,
                                              .onClick = [self] {
                                                self->m_capture = Idle{};
                                                self->change([](Settings& x) { x.keys = Keybindings{}; });
                                              }});
                         }}}});
      }
    }
    if (m_actions.openPath) {
      out.push_back({Pane::Main,
                     i18n::tr("terminal.prefs.settings-file"),
                     {{m_store.path().filename().string(), tildePath(m_store.path()), [self](float s, float) -> std::unique_ptr<Node> {
                         return ui::button({.text = i18n::tr("terminal.prefs.open"),
                                            .fontSize = Tokens::typeFootnote.size * s,
                                            .variant = ButtonVariant::Ghost,
                                            .radius = Style::scaledRadiusSm(s),
                                            .height = Tokens::sizeControlS * s,
                                            .onClick = [self] { self->m_actions.openPath(self->m_store.path()); }});
                       }}}});
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
      // An InputArea does not stretch its child: a clickable row gets the card's inner width itself.
      const float rowW = width - Style::cardPadding * s * 2.0F;
      auto line = ui::row({.align = FlexAlign::Center,
                           .gap = Tokens::spaceP12 * s,
                           .paddingV = Tokens::spaceP8 * s,
                           .paddingH = padH,
                           .minHeight = Tokens::sizeRow * s,
                           .width = row.onClick ? std::optional(rowW) : std::nullopt},
                          std::move(text), row.control(s, inner));
      if (!row.onClick) {
        box->addChild(std::move(line));
        continue;
      }
      box->addChild(ui::inputArea({.cursorShape = ::CursorShape::Pointer,
                                   .width = rowW,
                                   .height = Tokens::sizeRow * s,
                                   .onClick = [click = row.onClick](const InputArea::PointerData&) { click(); }},
                                  std::move(line)));
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
    // Opaque, so the page under it does not show through during a page slide.
    scroll->setFill(colorSpecFromRole(ColorRole::SolidRegular));
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
    const auto lineOf = [](int line) { return line > 0 ? i18n::tr("terminal.prefs.error-line", "line", line) : std::string(); };

    if (const auto* invalid = std::get_if<SettingsStore::Invalid>(&m_store.fileState())) {
      errorCard(i18n::tr("terminal.prefs.file-error", "name", m_store.path().filename().string()),
                lineOf(invalid->error.line) + invalid->error.message + i18n::tr("terminal.prefs.last-good-settings"), i18n::tr("terminal.prefs.show-file"),
                open(m_store.path()));
    }

    const std::vector<Group> all = groups();
    const auto themeErrors = [&] {
      if (m_importError) {
        errorCard(i18n::tr("terminal.prefs.import-failed", "name", m_importError->folder),
                  lineOf(m_importError->error.line) + m_importError->error.message + ".", i18n::tr("terminal.prefs.try-again"),
                  m_actions.importPackage);
      }
      for (const ThemeEntry& entry : m_store.themes().entries()) {
        if (const auto* invalid = std::get_if<ThemeEntry::Invalid>(&entry.state)) {
          errorCard(i18n::tr("terminal.prefs.theme-error", "name", entry.id),
                    lineOf(invalid->error.line) + invalid->error.message
                        + (invalid->lastGood ? i18n::tr("terminal.prefs.last-good-theme") : "."),
                    invalid->error.line > 0 ? i18n::tr("terminal.prefs.show-file") : i18n::tr("terminal.prefs.show-folder"),
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
          hits.title = i18n::tr("terminal.prefs.keyboard");
        }
        if (!hits.rows.empty()) {
          addGroup(hits, !any);
          any = true;
        }
      }
      if (!any) {
        auto empty = ui::column({.align = FlexAlign::Center, .paddingV = Tokens::spaceP48 * s, .width = cardWidth});
        empty->addChild(ui::label({.text = i18n::tr("terminal.prefs.no-results", "query", m_query),
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
      back->addChild(ui::label({.text = i18n::tr("terminal.prefs.title"),
                                .fontSize = Tokens::typeBody.size * s,
                                .fontWeight = FontWeight::Medium,
                                .color = colorSpecFromRole(ColorRole::AccentText)}));
      content->addChild(ui::inputArea({.cursorShape = ::CursorShape::Pointer,
                                       .width = cardWidth,
                                       .height = Tokens::sizeRow * s,
                                       .onClick = [this](const InputArea::PointerData&) { showPane(Pane::Main); }},
                                      std::move(back)));
      auto title = ui::column({.align = FlexAlign::Start, .paddingH = Tokens::spaceP12 * s});
      title->addChild(ui::label({.text = i18n::tr("terminal.prefs.keyboard"),
                                 .fontSize = Tokens::typeTitle3.size * s,
                                 .fontWeight = FontWeight::SemiBold,
                                 .color = colorSpecFromRole(ColorRole::Label1)}));
      content->addChild(std::move(title));
      auto hint = ui::column({.align = FlexAlign::Start, .paddingH = Tokens::spaceP12 * s});
      hint->addChild(ui::label({.text = i18n::tr("terminal.prefs.shortcuts-help"),
                                .fontSize = Tokens::typeFootnote.size * s,
                                .color = colorSpecFromRole(ColorRole::Label2),
                                .maxWidth = cardWidth - Tokens::spaceP12 * s * 2.0F}));
      content->addChild(std::move(hint));
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

  std::unique_ptr<Node> PrefsWindow::preview(Renderer& renderer, float s, float width, float height) const {
    const Settings& now = m_store.settings();
    const ThemeSpec theme = resolveTheme(now.theme, m_store.themes());
    const std::string family = gridFont(now, theme);
    const float size = static_cast<float>(now.fontSize) * s;
    const ThemePalette colors = themePalette(theme);
    const bool scene = hasScene(theme);
    const float radius = Style::scaledRadius(Tokens::componentWindowCardRadius, s);
    // The card stands for the main pane: the theme's scene under its tint, or the grid's glass alpha.
    auto box = ui::column({.align = FlexAlign::Stretch,
                           .fill = fixedColorSpec(withAlpha(colors.view, scene ? Tokens::componentTerminalSceneTint
                                                                               : backgroundAlpha(now, theme))),
                           .radius = radius,
                           .border = colorSpecFromRole(ColorRole::Separator),
                           .borderWidth = Style::borderWidth * s,
                           .clipChildren = scene,
                           .width = width,
                           .height = height});
    if (scene) {
      box->setClipRadii(Radii{radius, radius, radius, radius});
      // One still frame: the preview runs no scene clock.
      std::unique_ptr<Node> backdrop;
      const auto& source = std::get<ThemePackage>(theme).backdrop;
      if (const auto* picture = std::get_if<ThemePackage::Picture>(&source)) {
        auto image = std::make_unique<Image>();
        image->setFit(ImageFit::Cover);
        (void)image->setSourceFile(renderer, picture->path, static_cast<int>(std::ceil(std::max(width, height))));
        image->setSize(width, height);
        backdrop = std::move(image);
      } else {
        auto shader = std::make_unique<ShaderSceneNode>();
        shader->setSource(std::get<ThemePackage::Shader>(source).source);
        shader->setColors(colors.view, colors.label1, colors.accent, !isResolvedLightTheme());
        shader->setFrameSize(width, height);
        backdrop = std::move(shader);
      }
      backdrop->setParticipatesInLayout(false);
      backdrop->setZIndex(-1);
      box->addChild(std::move(backdrop));
    }
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
    // The prompt's context line: a band with the path and branch, the last command's result at the end.
    const auto context = [&](std::string result, bool first) {
      const float h = std::round(size * Tokens::typeMono.line / Tokens::typeMono.size);
      auto pill = [&] {
        return ui::row({.align = FlexAlign::Center,
                        .gap = Tokens::spaceP4 * s,
                        .paddingH = Tokens::spaceP8 * s,
                        .fill = fixedColorSpec(lerpColor(colors.view, colors.label1, 0.04F)),
                        .radius = Style::scaledRadius(Tokens::radiusR8, s),
                        .minHeight = h});
      };
      if (!first) {
        box->addChild(ui::box({.height = size * 1.4F}));
      }
      auto row = ui::row({.align = FlexAlign::Center, .minHeight = h});
      auto band = pill();
      band->addChild(ui::label({.text = "~/src/desktop ", .fontSize = size, .fontFamily = family, .color = fixedColorSpec(colors.ansi[4])}));
      band->addChild(ui::label({.text = "main", .fontSize = size, .fontFamily = family, .color = fixedColorSpec(colors.label2)}));
      row->addChild(std::move(band));
      if (!result.empty()) {
        row->addChild(ui::column({.flexGrow = 1.0F}));
        auto tail = pill();
        tail->addChild(ui::label({.text = std::move(result), .fontSize = meta, .color = fixedColorSpec(colors.label2)}));
        row->addChild(std::move(tail));
      }
      box->addChild(std::move(row));
    };
    // The prompt as the shell integration draws it: the brand mark, then the command.
    const auto prompt = [&](std::string command, const Color& ink) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = 0.0F, .clipChildren = true, .width = inner});
      auto mark = std::make_unique<Image>();
      mark->setSize(size, size);
      mark->setForegroundTint(fixedColorSpec(colors.accent));
      (void)mark->setSourceFile(renderer, paths::assetPath("amark.svg").string(), static_cast<int>(std::lround(size)));
      row->addChild(std::move(mark));
      row->addChild(ui::label({.text = " " + std::move(command), .fontSize = size, .fontFamily = family,
                               .color = fixedColorSpec(ink), .maxLines = 1}));
      box->addChild(std::move(row));
    };
    const char* cursor = now.cursor == CursorShape::Bar ? "▏" : now.cursor == CursorShape::Underline ? "▁" : "█";
    context("2.4 s", true);
    prompt("cargo test -p service", colors.label1);
    line({{"   Compiling service v0.4.1 (/src/desktop/service)", colors.label2}});
    line({{"test service::accepts_signed_index ... ", colors.label1}, {"ok", colors.ansi[2]}});
    line({{"warning", colors.ansi[3]}, {": unused import ", colors.label1}, {"`std::io`", colors.ansi[6]}});
    line({{"test service::resumes_partial ... ", colors.label1}, {"FAILED", colors.ansi[1]}});
    context("exit 101 · 0.8 s", false);
    prompt("git log --oneline -2", colors.label1);
    line({{"e1e5790", colors.ansi[3]}, {" Terminal themes show their own colours", colors.label1}});
    line({{"4873f2d", colors.ansi[3]}, {" Terminal theme Import…", colors.label1}});
    context("", false);
    prompt(cursor, colors.accent);

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
                                    + std::string(materialName(now.material)),
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
                                          float tileW, float tileH, bool ownRing,
                                          std::function<void(const InputArea::PointerData&)> pick) {
    const bool ring = on && ownRing;
    swatch->setBorder(colorSpecFromRole(ring ? ColorRole::Label1 : ColorRole::Separator),
                      (ring ? Style::focusRingWidth : Style::borderWidth) * s);
    auto column = ui::column({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .width = tileW});
    column->addChild(checked(std::move(swatch), on, s));
    column->addChild(ui::label({.text = std::string(name),
                                .fontSize = Tokens::typeFootnote.size * s,
                                .fontWeight = on ? FontWeight::SemiBold : FontWeight::Normal,
                                .color = colorSpecFromRole(on ? ColorRole::Label1 : ColorRole::Label2),
                                .maxWidth = tileW,
                                .maxLines = 1}));
    return ui::inputArea({.cursorShape = ::CursorShape::Pointer,
                          .width = tileW,
                          .height = tileH + Tokens::spaceP8 * s + Tokens::typeFootnote.line * s,
                          .onClick = [pick = std::move(pick)](const InputArea::PointerData& data) { pick(data); }},
                         std::move(column));
  }

  std::unique_ptr<Node> PrefsWindow::materialTiles(float s, float width) {
    constexpr std::array kMaterials{WindowMaterial::Solid, WindowMaterial::Glass, WindowMaterial::GlassSidebar,
                                    WindowMaterial::GlassContent};
    const float gap = Tokens::spaceP12 * s;
    const float tileW = std::floor((width - gap) / 2.0F);
    const float tileH = std::round(tileW * Tokens::componentTerminalWindowTileAspect);
    const float inset = Tokens::spaceP12 * s;
    const WindowMaterial current = m_store.settings().material;
    auto grid = ui::column({.gap = gap, .width = width});
    std::unique_ptr<Flex> row;
    for (const WindowMaterial material : kMaterials) {
      if (!row) {
        row = ui::row({.gap = gap, .width = width});
      }
      const std::string_view name = materialName(material);
      const bool on = material == current;
      const float innerW = tileW - inset * 2.0F;
      const float sideW = std::round(innerW * Tokens::componentTerminalWindowTileSidebar);
      const float r = Style::scaledRadiusSm(s);
      auto window = ui::row({.width = innerW, .height = tileH - inset * 2.0F});
      const float tint = Style::glassMaterial(glass::Kind::Sheet).tint;
      auto side = ui::column({.fill = colorSpecFromRole(ColorRole::SolidThin, glassSidebar(material) ? tint : 1.0F),
                              .fillHeight = true,
                              .width = sideW});
      side->setRadii(Radii{r, 0.0F, 0.0F, r});
      auto main = ui::column({.fill = colorSpecFromRole(ColorRole::SolidRegular, glassContent(material) ? tint : 1.0F),
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
      row->addChild(tile(std::move(swatch), name, on, s, tileW, tileH, true,
                         [this, material](const InputArea::PointerData&) {
                           change([material](Settings& x) { x.material = material; });
                         }));
      if (row->children().size() == 2) {
        grid->addChild(std::move(row));
      }
    }
    return grid;
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
      row->addChild(tile(std::move(swatch), choice.name, choice.id == current, s, tileW, tileH, false,
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
    row->addChild(tile(std::move(add), i18n::tr("terminal.prefs.add-theme"), false, s, tileW, tileH, false, [this](const InputArea::PointerData& at) {
      enum : std::int32_t { kCreate = 1, kImport, kFolder };
      m_window.openContextMenu({{.id = kCreate, .label = i18n::tr("terminal.prefs.create-with-claude"), .glyph = "robot"},
                                {.id = kImport, .label = i18n::tr("terminal.prefs.import"), .glyph = "file-import"},
                                {.id = kFolder, .label = i18n::tr("terminal.prefs.open-themes-folder"), .glyph = "folder"}},
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

    // The selection ring is one node over the grid, so a new pick moves it from the old tile (P1 lens travel).
    const auto at = std::ranges::find(choices, current, &ThemeChoice::id);
    if (at == choices.end()) {
      m_ringAt.reset();
      return grid;
    }
    const auto index = static_cast<std::size_t>(at - choices.begin());
    const std::size_t rows = (choices.size() + kPerRow) / kPerRow; // the Add theme tile included
    const float cellH = tileH + Tokens::spaceP8 * s + Tokens::typeFootnote.line * s;
    const float rowGap = Tokens::spaceP12 * s;
    const std::pair to{static_cast<float>(index % kPerRow) * (tileW + gap), static_cast<float>(index / kPerRow) * (cellH + rowGap)};
    auto layers = slideSlot(width, static_cast<float>(rows) * cellH + static_cast<float>(rows - 1) * rowGap);
    layers->setClipChildren(false);
    layers->addChild(std::move(grid));
    auto ring = ui::column({.radius = Style::scaledRadiusSm(s),
                            .border = colorSpecFromRole(ColorRole::Label1),
                            .borderWidth = Style::focusRingWidth * s,
                            .width = tileW,
                            .height = tileH});
    ring->setParticipatesInLayout(false);
    Node* ringNode = layers->addChild(std::move(ring));
    SlideSlot* slot = layers.get();
    const std::pair from = m_ringAt.value_or(to);
    m_ringAt = to;
    slot->setSlide(ringNode, from.first, from.second);
    if (from != to && !MotionService::instance().reduced()) {
      m_window.animations().drive(0, 0.0F, 1.0F, Style::spring(Tokens::springSnap),
                                  [this, slot, ringNode, from, to](float t) {
                                    slot->setSlide(ringNode, std::lerp(from.first, to.first, t), std::lerp(from.second, to.second, t));
                                    m_window.invalidate();
                                  },
                                  {}, ringNode);
    } else {
      slot->setSlide(ringNode, to.first, to.second);
    }
    return layers;
  }

} // namespace term
