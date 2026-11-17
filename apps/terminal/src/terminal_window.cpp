#include "terminal_window.h"

#include "agent_host.h"

#include "app/app_loop.h"
#include "app/app_window.h"
#include "notifier.h"
#include "shell_integration.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "time/time_format.h"
#include "render/animation/animation_manager.h"
#include "render/text/glyph_registry.h"
#include "ui/builders.h"
#include "ui/controls/selection_list.h"
#include "ui/controls/status_row.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/glyph.h"
#include "ui/controls/spinner.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/contrast.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/drag_and_drop.h"
#include "wayland/wayland_connection.h"

#include "cursor-shape-v1-client-protocol.h"

#include <gio/gio.h>
#include <pwd.h>
#include <unistd.h>

#include <linux/input-event-codes.h>

#include <algorithm>
#include <csignal>
#include <format>
#include <ranges>
#include <cctype>
#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace term {

  static_assert(KeyMod::Shift == 1U && KeyMod::Ctrl == 2U && KeyMod::Alt == 4U && KeyMod::Super == 8U,
                "keybindings.cpp mirrors the KeyMod bits");

  namespace {
    std::optional<AppWindow::Scene> windowScene(const SettingsStore& settings) {
      const ThemeSpec theme = resolveTheme(settings.settings().theme, settings.themes());
      if (!hasScene(theme)) {
        return std::nullopt;
      }
      const ThemePalette colors = themePalette(theme);
      // The grid and the prompt band draw ANSI colours too; one that is weak even on the plain view is left out.
      std::vector<Color> inks{colors.label2};
      for (std::size_t i = 1; i < colors.ansi.size(); ++i) {
        if (contrastRatio(colors.ansi[i], colors.view) >= Tokens::componentTerminalSceneInkContrast) {
          inks.push_back(colors.ansi[i]);
        }
      }
      const auto& backdrop = std::get<ThemePackage>(theme).backdrop;
      const auto* picture = std::get_if<ThemePackage::Picture>(&backdrop);
      return AppWindow::Scene{.backdrop = picture != nullptr
                                              ? AppWindow::Scene::Backdrop{AppWindow::Scene::Picture{picture->path, picture->stamp}}
                                              : AppWindow::Scene::Backdrop{AppWindow::Scene::Shader{std::get<ThemePackage::Shader>(backdrop).source}},
                              .surface = colors.view, .text = colors.label1,
                              .accent = colors.accent, .inks = std::move(inks), .dark = !isResolvedLightTheme(),
                              .tint = Tokens::componentTerminalSceneTint};
    }
  } // namespace

  namespace {
    constexpr Logger kLog("terminal");

    // The status comes first, so truncation removes the command, never the outcome.
    std::string withCommand(const std::string& status, const std::string& command) {
      return command.empty() ? status : status + " · " + command;
    }

    // A caption sits on the command's own output, so a finished state names how long it took, not the command.
    std::string captionState(const SessionInfo& session, const std::string& rowStatus) {
      const auto* finished = std::get_if<CommandFinished>(&session.command);
      if (finished == nullptr || session.kind == SessionKind::Agent) {
        return rowStatus;
      }
      return (finished->failed() ? "exit " + std::to_string(*finished->exitCode) : std::string("done")) + " · "
          + formatTook(finished->duration);
    }

    std::string homeDir() {
      const char* home = std::getenv("HOME");
      return home != nullptr ? home : "/";
    }

    // A path for the sidebar and title: $HOME shows as ~.
    std::string shownPath(const std::string& path) {
      const std::string home = homeDir();
      const bool inHome = path == home || path.starts_with(home + "/");
      return inHome ? "~" + path.substr(home.size()) : path;
    }
  } // namespace

  TerminalWindow::TerminalWindow(AppLoop& loop, AppWindow& window, Notifier& notifier, AgentHosts agents,
                                 ThumbnailService& thumbnails, SettingsStore& settings,
                                 const std::vector<WindowOrigin>& origins, std::function<void()> close,
                                 std::function<void(LayoutSave)> sessionsChanged,
                                 std::function<void(const std::string& token, std::vector<WindowOrigin> origins)> newWindow,
                                 std::function<void(const std::string& token)> preferences, ChooseSaveFile chooseSaveFile)
      : m_loop(loop), m_window(window), m_notifier(notifier), m_agents(std::move(agents)), m_thumbnails(thumbnails), m_settings(settings), m_close(std::move(close)), m_sessionsChanged(std::move(sessionsChanged)),
        m_newWindow(std::move(newWindow)), m_preferences(std::move(preferences)),
        m_chooseSaveFile(std::move(chooseSaveFile)), m_split(window, splitCallbacks()) {
    m_settingsChanged = m_settings.changed().connect([this] {
      m_window.setMaterial(appMaterial(m_settings.settings().material));
      applyViewSettings();
      if (ContentView* view = shownView(); view != nullptr) {
        m_window.setMainFill(view->surface());
      }
      m_window.requestRebuild();
      m_split.repaintFocus();
    });
    m_window.setSceneSource([this] { return windowScene(m_settings); });
    // Split cards are placed for the content width at build: the settled sidebar width needs a new layout.
    m_window.setSidebarSettled([this] { m_window.requestRebuild(); });
    // The content is a slot: a session switch swaps the view inside it in place, so the sidebar keeps its nodes.
    m_window.setContentBuilder([this](Renderer& renderer, float width, float height) -> std::unique_ptr<Node> {
      m_bandStatus = nullptr; // the header rebuilds after the content and sets it again
      closePalette(true);
      for (Session& session : m_views) {
        session.view->sceneDestroyed();
      }
      auto slot = ui::column({.align = FlexAlign::Stretch, .width = width, .height = height});
      m_contentSlot = slot.get();
      m_contentWidth = width;
      m_contentHeight = height;
      fillSlot(renderer);
      registerDropTarget();
      return slot;
    });
    m_window.setSidebarBuilder([this](Renderer& /*renderer*/, float width, float height) -> std::unique_ptr<Node> {
      const float s = m_window.uiScale();
      const float button = Tokens::sizeControlM * s;
      auto side = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s, .padding = Tokens::spaceP8 * s,
                              .width = width, .height = height});
      // Under the window's compact width the sidebar is an icon rail (as in Files): "+" and session symbols only.
      m_rail = width < 120.0F * s;
      auto top = ui::row({.align = FlexAlign::Center, .justify = m_rail ? FlexJustify::Center : FlexJustify::Start,
                          .gap = Tokens::spaceP8 * s});
      m_search = nullptr;
      if (m_rail) {
        m_filter.clear();
      } else {
        top->addChild(ui::searchCapsule({
            .out = &m_search,
            .placeholder = "Sessions",
            .scale = s,
            .value = m_filter,
            .flexGrow = 1.0F,
            .onChange = [this](const std::string& text) { setFilter(text); },
            .onSubmit = [this](const std::string& /*text*/) { openFirstMatch(); },
            .onKeyEvent = [this](std::uint32_t sym, std::uint32_t /*mods*/) {
              constexpr std::uint32_t kEscape = 0xff1b;
              if (sym != kEscape) {
                return false;
              }
              focusGrid();
              return true;
            },
        }));
      }
      // A click opens a shell session; a right-click offers the other kinds.
      top->addChild(ui::button({.out = &m_newButton, .glyph = std::string("plus"), .glyphSize = Tokens::sizeSymbol * s,
                                .variant = ButtonVariant::Ghost, .tooltip = std::string("New session"),
                                .radius = button / 2.0F, .height = button,
                                .onClick = [this] { openSession(NewShell{}); }}));
      m_newButton->setOnRightClick([this] { openNewMenu(); });
      side->addChild(std::move(top));
      auto list = std::make_unique<SelectionList>(s);
      if (const ThemeSpec theme = resolveTheme(m_settings.settings().theme, m_settings.themes()); hasScene(theme)) {
        list->setTint(fixedColorSpec(withAlpha(themePalette(theme).label1, Tokens::componentTerminalSceneSelection)));
      } else {
        list->setTint(fixedColorSpec(withAlpha(themePalette(theme).accent, SelectionList::selectionTintAlpha())));
      }
      list->setGap(Tokens::spaceP4 * s);
      m_rows.clear();
      m_groups.clear();
      m_tintTarget = 0;
      // A split shows as one block of short rows in pane order, where its first session is in the list.
      for (const std::uint64_t id : sidebarOrder()) {
        const auto* split = m_sessions.splitOf(id);
        if (split == nullptr) {
          auto row = makeRow(id);
          m_rows.push_back({id, row.get()});
          list->addChild(std::move(row));
          continue;
        }
        if (split->front() != id) {
          continue;
        }
        auto group = ui::column({.align = FlexAlign::Stretch, .padding = Tokens::spaceP4 * s,
                                 .radius = Style::scaledRadius(Tokens::radiusR12, s)});
        for (const std::uint64_t member : *split) {
          auto row = makeRow(member);
          row->setPadding(0.0F, Tokens::spaceP8 * s);
          row->setMinHeight(Tokens::componentTerminalGroupRow * s);
          m_rows.push_back({member, row.get()});
          group->addChild(std::move(row));
        }
        m_groups.push_back({id, group.get()});
        list->addChild(std::move(group));
      }
      m_rowList = list.get();
      // The rows scroll under the fixed search bar; the gear stays at the bottom.
      auto scroll = ui::scrollView({.state = &m_sidebarScroll, .contentScale = s, .viewportPaddingH = 0.0F,
                                    .viewportPaddingV = 0.0F, .flexGrow = 1.0F});
      scroll->content()->setAlign(FlexAlign::Stretch);
      scroll->content()->addChild(std::move(list));
      side->addChild(std::move(scroll));
      side->addChild(ui::box({.out = &m_dropLine, .fill = colorSpecFromRole(ColorRole::Accent),
                              .radius = Tokens::componentTerminalSidebarDropLine * s / 2.0F, .visible = false,
                              .participatesInLayout = false}));
      auto bottom = ui::row({.align = FlexAlign::Center, .justify = m_rail ? FlexJustify::Center : FlexJustify::Start});
      bottom->addChild(ui::button({.glyph = std::string("settings"), .glyphSize = Tokens::sizeSymbol * s,
                                   .variant = ButtonVariant::Ghost, .tooltip = std::string("Preferences"),
                                   .radius = button / 2.0F, .height = button,
                                   .onClick = [this] { m_preferences(m_loop.wayland().requestActivationToken(m_window.wlSurface())); }}));
      side->addChild(std::move(bottom));
      refreshRows();
      applyFilter();
      return side;
    });
    m_window.setCaptionMenu([this](float x, float y) {
      if (const auto id = m_sessions.shown()) {
        openRowMenu(*id, x, y);
      }
    });
    m_window.setHeaderAccessoryBuilder([this]() -> std::unique_ptr<Node> {
      const float s = m_window.uiScale();
      const float button = Tokens::spaceP24 * s;
      auto slot = ui::row({.align = FlexAlign::Center, .flexGrow = 1.0F});
      auto bar = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .flexGrow = 1.0F});
      auto field = ui::row({.align = FlexAlign::Center, .maxWidth = Tokens::componentTerminalFindWidth * s, .flexGrow = 1.0F});
      field->addChild(ui::searchCapsule({
          .out = &m_findInput,
          .placeholder = "Find",
          .scale = s,
          .value = m_findQuery,
          .flexGrow = 1.0F,
          .onChange =
              [this](const std::string& text) {
                m_findQuery = text;
                if (TerminalView* view = shownTerminal(); view != nullptr) {
                  view->setFindQuery(text, m_findOptions);
                }
                if (text.empty()) {
                  setFindCount(0, std::nullopt, FindError::None);
                }
              },
          .onSubmit =
              [this](const std::string& /*text*/) {
                if (TerminalView* view = shownTerminal(); view != nullptr) {
                  view->findStep(true);
                }
              },
          .onKeyEvent = [this](std::uint32_t sym, std::uint32_t mods) {
            constexpr std::uint32_t kEscape = 0xff1b;
            constexpr std::uint32_t kReturn = 0xff0d;
            if (sym == kEscape) {
              closeFind();
              return true;
            }
            if (sym == kReturn && (mods & KeyMod::Shift) != 0) {
              if (TerminalView* view = shownTerminal(); view != nullptr) {
                view->findStep(false);
              }
              return true;
            }
            const bool altOnly = (mods & (KeyMod::Ctrl | KeyMod::Shift | KeyMod::Alt | KeyMod::Super)) == KeyMod::Alt;
            if (altOnly && (sym == 'c' || sym == 'r')) {
              toggleFindOption(sym == 'r');
              return true;
            }
            return false;
          },
      }));
      bar->addChild(std::move(field));
      const auto toggle = [&](const char* glyph, const char* tooltip, bool on, Button** out, bool regex) {
        return ui::button({.out = out, .glyph = std::string(glyph), .glyphSize = Tokens::spaceP12 * s, .controlHeight = button, .selected = on,
                           .variant = ButtonVariant::Ghost, .tooltip = std::string(tooltip),
                           .radius = button / 2.0F, .width = button, .height = button,
                           .onClick = [this, regex] { toggleFindOption(regex); }});
      };
      bar->addChild(toggle("letter-case", "Match case  Alt+C", m_findOptions.matchCase, &m_findToggles[0], false));
      bar->addChild(toggle("regex", "Regular expression  Alt+R", m_findOptions.regex, &m_findToggles[1], true));
      bar->addChild(ui::label({.out = &m_findCount, .fontSize = Tokens::typeFootnote.size * s,
                               .fontFeatures = std::string("tnum"), .color = colorSpecFromRole(ColorRole::Label2),
                               .maxLines = 1}));
      const auto round = [&](const char* glyph, const char* tooltip, Button** out, std::function<void()> onClick) {
        return ui::button({.out = out, .glyph = std::string(glyph), .glyphSize = Tokens::spaceP12 * s, .controlHeight = button,
                           .variant = ButtonVariant::Ghost, .tooltip = std::string(tooltip), .radius = button / 2.0F,
                           .width = button, .height = button, .onClick = std::move(onClick)});
      };
      bar->addChild(round("chevron-up", "Previous match  Shift+Enter", &m_findSteps[0], [this] {
        if (TerminalView* view = shownTerminal(); view != nullptr) {
          view->findStep(false);
        }
      }));
      bar->addChild(round("chevron-down", "Next match  Enter", &m_findSteps[1], [this] {
        if (TerminalView* view = shownTerminal(); view != nullptr) {
          view->findStep(true);
        }
      }));
      bar->addChild(round("x", "Close  Escape", nullptr, [this] { closeFind(); }));
      m_findBar = bar.get();
      m_findBar->setVisible(m_findOpen);
      m_findBar->setParticipatesInLayout(m_findOpen);
      slot->addChild(std::move(bar));
      auto agentBar = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .flexGrow = 1.0F});
      m_agentBar = agentBar.get();
      fillAgentBar();
      slot->addChild(std::move(agentBar));
      m_window.setHeaderSearchMode(m_findOpen);
      setFindCount(0, std::nullopt, FindError::None);
      return slot;
    });
    m_window.setHeaderAccessoryWidths([this](Renderer& renderer) {
      const float floor = m_agentBar->measure(renderer, {}).width;
      return AppWindow::HeaderAccessoryWidths{
          .floor = floor,
          .keep = m_agentBar->children().empty() ? floor : AgentView::bandKeep(*m_agentBar->children().front(), renderer)};
    });
    m_window.setFirstKeyHandler([this](const KeyboardEvent& event) {
      ContentView* view = shownView();
      // A click on a button leaves focus nowhere: keys go back to the shown session.
      if (view != nullptr && view->focusArea() != nullptr && m_window.input().focusedArea() == nullptr) {
        m_window.input().setFocus(view->focusArea());
      }
      if (sessionKey(event)) {
        return true;
      }
      return view != nullptr && view->focusArea() != nullptr && m_window.input().focusedArea() == view->focusArea() &&
             view->key(event);
    });
    m_window.setActiveCallback([this](bool active) {
      m_active = active;
      for (Session& session : m_views) {
        if (std::ranges::contains(m_builtPanes, session.id)) {
          session.view->setWindowActive(active);
        }
      }
    });
    m_window.setLayoutCallback([this] {
      if (!m_split.split()) {
        const AppWindow::Rect rect = m_window.contentRect();
        if (ContentView* view = shownView(); view != nullptr) {
          view->setSize(rect.width, rect.height);
        }
        return;
      }
      if (m_split.dragging()) {
        return;
      }
      for (const auto& [id, r] : m_split.contentRects()) {
        if (const auto it = std::ranges::find(m_views, id, &Session::id); it != m_views.end()) {
          it->view->setSize(r.width, r.height);
        }
      }
    });
    // A divider drag resizes the cards per frame; the views (and their PTYs) resize once, on release.
    // A click into a pane's view focuses its area: that pane becomes the shown session.
    m_window.input().setFocusChangeCallback([this](InputArea* /*old*/, InputArea* next) {
      for (const auto& [card, rect] : m_split.contentRects()) {
        const auto it = std::ranges::find(m_views, card, &Session::id);
        if (next != nullptr && it != m_views.end() && it->view->focusArea() == next && m_sessions.shown() != card) {
          DeferredCall::callLater([this, id = card, alive = std::weak_ptr<bool>(m_alive)] {
            if (!alive.expired()) {
              apply(session_event::Show{id});
            }
          });
        }
      }
    });
    for (const WindowOrigin& origin : origins) {
      openSession(origin);
    }
  }

  TerminalView::Host TerminalWindow::terminalHost(std::uint64_t id) {
    auto alive = std::weak_ptr<bool>(m_alive);
    return TerminalView::Host{
        .requestFrame = [this] { requestFrame(); },
        .playBell = [this] { m_loop.wayland().ringBell(m_window.wlSurface()); },
        .bellRang =
            [this, id, alive] {
              if (std::ranges::contains(m_sessions.visible(), id)) {
                m_window.pulseScene(AppWindow::ScenePulse::Bell);
              }
              // In the background, the bell marks the window as needing attention; it never takes focus.
              if (const ToplevelSurface* top = m_window.surface(); top != nullptr && !top->state().activated) {
                m_loop.wayland().requestAttentionTokenAsync(m_window.wlSurface(), [this, alive](std::string token) {
                  if (!alive.expired()) {
                    m_loop.wayland().activateSurface(m_window.wlSurface(), token);
                  }
                });
              }
            },
        .secretInput =
            [this, id](bool secret) {
              if (secret) {
                m_secretSessions.insert(id);
              } else {
                m_secretSessions.erase(id);
              }
              updateSceneState();
              refreshRows();
            },
        .textures = [this]() -> TextureManager* {
          RenderContext& render = m_loop.renderContext();
          return render.glReady() ? &render.textureManager() : nullptr;
        },
        .thumbnails = &m_thumbnails,
        // exited runs inside the view's frame(): the view is destroyed later.
        .exited =
            [this, id, alive](int status) {
              // A logout kills the shells with the app: the session stays in the layout a restore reopens.
              const bool killed = status == 128 + SIGHUP || status == 128 + SIGTERM || status == 128 + SIGKILL;
              DeferredCall::callLater([this, id, alive, killed] {
                if (!alive.expired()) {
                  apply(session_event::Close{id}, killed ? LayoutSave::Unsaved : LayoutSave::Saved);
                }
              });
            },
        .modifiers = [this] { return m_loop.wayland().keyboardModifiers(); },
        .notify =
            [this, id](const Notice& notice, std::function<void(const std::string&, int)> clicked,
                       std::function<void()> closed) {
              if (notice.when == Notice::When::Invisible && m_active && std::ranges::contains(m_sessions.visible(), id)) {
                return;
              }
              m_notifier.notify(
                  notice, notice.key.empty() ? std::string{} : std::format("{}/{}", id, notice.key), std::move(clicked),
                  std::move(closed));
            },
        .focus = [this](const std::string& token) { m_window.activate(token); },
        .closeNotice = [this, id](const std::string& key) { m_notifier.close(std::format("{}/{}", id, key)); },
        .confirmPaste = [this](const std::string& text, std::function<void()> paste) { askPaste(text, std::move(paste)); },
        .titleChanged = [this, id](std::string title) { apply(session_event::Title{id, std::move(title)}); },
        .commandStarted =
            [this, id](const std::string& command) {
              apply(session_event::CommandStart{id, command, SessionClock::now()});
            },
        .commandEnded =
            [this, id](std::optional<int> exitCode) {
              apply(session_event::CommandEnd{id, exitCode, SessionClock::now()});
            },
        .commandOutput = [this, id](CommandOutput output) { apply(session_event::Output{id, output}); },
        .cwdChanged =
            [this, id](const std::string& path) {
              const SessionInfo* info = m_sessions.find(id);
              // The first cwd of a new session is no change.
              if (info != nullptr && !info->cwd.empty() && std::ranges::contains(m_sessions.visible(), id)) {
                m_window.pulseScene(AppWindow::ScenePulse::Cwd);
              }
              const std::string shown = shownPath(path);
              apply(session_event::Cwd{id, shown});
            },
        .hostChanged =
            [this, id](const std::optional<Remote>& remote, bool moved) {
              if (remote) {
                m_remoteSessions.insert(id);
              } else {
                m_remoteSessions.erase(id);
              }
              if (moved && std::ranges::contains(m_sessions.visible(), id)) {
                m_window.pulseScene(AppWindow::ScenePulse::Host);
              }
              const SessionInfo* info = m_sessions.find(id);
              if (info != nullptr && info->kind != SessionKind::App && info->kind != SessionKind::Agent) {
                const SessionKind kind = !remote ? SessionKind::Shell
                    : remote->kind == Remote::Kind::Ssh ? SessionKind::Ssh : SessionKind::Container;
                apply(session_event::Kind{id, kind, remote ? remote->host.value_or("") : ""});
              }
              updateSceneState();
            },
        .openUri = [this](const std::string& uri) { openUri(uri); },
        .findChanged =
            [this, id](std::size_t total, std::optional<std::size_t> current, FindError error) {
              if (m_sessions.shown() == id) {
                setFindCount(total, current, error);
              }
            },
        .textBacking = [this] { return m_window.sceneTextBacking(); },
        .settings = viewSettings(),
        .theme = resolveTheme(m_settings.settings().theme, m_settings.themes())};
  }

  void TerminalWindow::adoptSession(std::uint64_t id, MovedSession moved) {
    if (TerminalView* terminal = moved.view->terminal(); terminal != nullptr) {
      terminal->setHost(terminalHost(id));
      terminal->setSecureInput(m_keysSecure, m_window.input());
      const Settings settings = viewSettings();
      terminal->applySettings(settings, resolveTheme(settings.theme, m_settings.themes()));
    } else if (AgentView* agent = moved.view->agent(); agent != nullptr) {
      agent->setOpenUri([this](const std::string& uri) { openUri(uri); });
    }
    if (moved.secret) {
      m_secretSessions.insert(id);
    }
    if (moved.remote) {
      m_remoteSessions.insert(id);
    }
    m_views.push_back({id, std::move(moved.view)});
    // The row keeps what the old window knew: name, place, host, title and its command.
    const SessionInfo& info = moved.info;
    apply(session_event::Open{id, info.kind, info.name, info.cwd});
    if (!info.host.empty()) {
      apply(session_event::Kind{id, info.kind, info.host});
    }
    if (!info.title.empty()) {
      apply(session_event::Title{id, info.title});
    }
    if (const auto* running = std::get_if<CommandRunning>(&info.command)) {
      apply(session_event::CommandStart{id, running->command, running->start});
    } else if (const auto* done = std::get_if<CommandFinished>(&info.command)) {
      const auto now = SessionClock::now();
      apply(session_event::CommandStart{id, done->command, now - done->duration});
      apply(session_event::CommandEnd{id, done->exitCode, now});
    }
  }

  void TerminalWindow::moveToNewWindow(std::uint64_t id) {
    const SessionInfo* info = m_sessions.find(id);
    const auto it = std::ranges::find(m_views, id, &Session::id);
    if (info == nullptr || it == m_views.end()) {
      return;
    }
    auto moved = std::make_shared<MovedSession>(MovedSession{.id = id, .view = std::move(it->view), .info = *info,
                                                             .secret = m_secretSessions.contains(id),
                                                             .remote = m_remoteSessions.contains(id)});
    m_views.erase(it);
    // Its nodes stay in this scene until the next build; the new window's build gives it new ones.
    moved->view->sceneDestroyed();
    apply(session_event::Close{id});
    m_newWindow(m_loop.wayland().requestActivationToken(m_window.wlSurface()), {Adopt{std::move(moved)}});
  }

  std::uint64_t TerminalWindow::openSession(const WindowOrigin& origin) {
    const auto* adopt = std::get_if<Adopt>(&origin);
    const std::uint64_t id = adopt != nullptr ? adopt->session->id : s_nextId++;
    if (adopt != nullptr) {
      adoptSession(id, std::move(*adopt->session));
      return id;
    }
    if (const auto* fresh = std::get_if<NewAgent>(&origin)) {
      const std::uint64_t key = s_nextAgentKey++;
      const std::string cwd = fresh->cwd.empty() ? homeDir() : fresh->cwd;
      agent::Host& host = m_agents(fresh->kind);
      agent::Session& session = host.open(key, cwd);
      if (!fresh->prompt.empty() && session.send(fresh->prompt)) {
        host.flush(key);
      }
      auto agentView = std::make_unique<AgentView>(host, key);
      agentView->setOpenUri([this](const std::string& uri) { openUri(uri); });
      m_views.push_back({id, std::move(agentView)});
      apply(session_event::Open{id, SessionKind::Agent, std::string(agent::info(fresh->kind).name), shownPath(cwd)});
      return id;
    }
    auto view = std::make_unique<TerminalView>(terminalHost(id));
    TerminalView& ref = *view;
    ref.setSecureInput(m_keysSecure, m_window.input());
    m_views.push_back({id, std::move(view)});
    const auto* reattach = std::get_if<Reattach>(&origin);
    if (reattach == nullptr || !ref.reattach(reattach->heldId)) {
      const auto* fresh = std::get_if<NewShell>(&origin);
      std::string cwd = fresh != nullptr ? fresh->cwd : std::string();
      if (cwd.empty() && m_settings.settings().startDir == StartDir::Current) {
        for (const Session& session : m_views) {
          if (const auto* shown = dynamic_cast<const TerminalView*>(session.view.get());
              shown != nullptr && session.id == m_sessions.shown()) {
            cwd = shown->cwd();
          }
        }
      }
      if (fresh != nullptr && !fresh->argv.empty()) {
        if (!ref.start(fresh->argv, cwd.empty() ? homeDir() : cwd, {})) {
          kLog.warn("cannot start {}", fresh->argv.front());
        }
      } else {
        const std::string shell = m_settings.settings().shell.empty() ? defaultShell() : m_settings.settings().shell;
        const ShellLaunch launch = shellLaunch(shell, shellIntegrationDir(), [](const char* name) { return std::getenv(name); });
        if (!ref.start(launch.argv, cwd.empty() ? homeDir() : cwd, launch.env)) {
          kLog.warn("cannot start {}", shell);
        }
      }
    }
    apply(session_event::Open{id, SessionKind::Shell, "", ""});
    return id;
  }

  SavedWindow TerminalWindow::layout() const {
    return saveLayout(m_sessions, [this](std::uint64_t id) {
      const auto it = std::ranges::find(m_views, id, &Session::id);
      const auto* view = it != m_views.end() ? dynamic_cast<const TerminalView*>(it->view.get()) : nullptr;
      return view != nullptr ? view->cwd() : std::string();
    });
  }

  void TerminalWindow::restore(const SavedWindow& saved) {
    std::vector<std::uint64_t> ids;
    for (const SavedSession& session : saved.sessions) {
      std::error_code error;
      // A folder removed since the save starts the shell in $HOME.
      ids.push_back(openSession(NewShell{.cwd = std::filesystem::is_directory(session.cwd, error) ? session.cwd : ""}));
    }
    for (const SessionListEvent& event : restoreEvents(saved, ids)) {
      apply(event);
    }
  }

  ContentView* TerminalWindow::shownView() {
    const auto id = m_sessions.shown();
    if (!id) {
      return nullptr;
    }
    const auto it = std::ranges::find(m_views, *id, &Session::id);
    return it == m_views.end() ? nullptr : it->view.get();
  }

  TerminalView* TerminalWindow::shownTerminal() {
    ContentView* view = shownView();
    return view != nullptr ? view->terminal() : nullptr;
  }

  void TerminalWindow::lockSymbol(std::uint64_t id, StatusRow::Data& data) const {
    if (m_secureEntry || m_secretSessions.contains(id)) {
      data.symbol = "lock";
      data.symbolDanger = false;
      data.spinning = false;
    }
  }

  void TerminalWindow::toggleSecureEntry() {
    m_secureEntry = !m_secureEntry;
    updateSceneState();
    refreshRows();
  }

  void TerminalWindow::updateSceneState() {
    const std::vector<std::uint64_t> visible = m_sessions.visible();
    const bool secret = std::ranges::any_of(visible, [this](std::uint64_t id) { return m_secretSessions.contains(id); });
    m_window.setSceneSecret(secret);
    const bool secure = m_secureEntry || secret;
    m_window.setCaptureSecure(secure);
    m_window.setShortcutsInhibited(secure);
    if (secure != m_keysSecure) {
      m_keysSecure = secure;
      for (const Session& session : m_views) {
        if (TerminalView* terminal = session.view->terminal(); terminal != nullptr) {
          terminal->setSecureInput(secure, m_window.input());
        }
      }
    }
    m_window.setSceneRemote(
        std::ranges::any_of(visible, [this](std::uint64_t id) { return m_remoteSessions.contains(id); }));
    m_window.setSceneBusy(std::ranges::any_of(visible, [this](std::uint64_t id) {
      const SessionInfo* info = m_sessions.find(id);
      return info != nullptr && std::holds_alternative<CommandRunning>(info->command);
    }));
  }

  void TerminalWindow::apply(const SessionListEvent& event, LayoutSave save) {
    const auto before = m_sessions.shown();
    const auto splitsBefore = m_sessions.splits();
    const ApplyResult result = m_sessions.apply(event);
    if (result == ApplyResult::Rejected) {
      return;
    }
    if (const auto* end = std::get_if<session_event::CommandEnd>(&event);
        end != nullptr && end->exitCode && std::ranges::contains(m_sessions.visible(), end->id)) {
      m_window.pulseScene(*end->exitCode == 0 ? AppWindow::ScenePulse::Success : AppWindow::ScenePulse::Failure);
    }
    if (const auto* close = std::get_if<session_event::Close>(&event)) {
      m_secretSessions.erase(close->id);
      m_remoteSessions.erase(close->id);
    }
    // Output is the frequent event and changes neither the visible panes nor their commands.
    if (!std::holds_alternative<session_event::Output>(event)) {
      updateSceneState();
    }
    // The sidebar's groups follow the splits: a rebuild regroups the rows (rare: a join, a leave, a member closed).
    const bool regroup = m_sessions.splits() != splitsBefore || std::holds_alternative<session_event::Move>(event);
    if (const auto* close = std::get_if<session_event::Close>(&event)) {
      // A moved session left m_views before its Close: no fade, nothing to retire.
      if (const auto it = std::ranges::find(m_views, close->id, &Session::id); it != m_views.end()) {
        m_split.noteClosing(close->id);
        if (std::ranges::contains(m_builtPanes, close->id)) {
          m_retired.push_back(std::move(*it));
        }
        m_views.erase(it);
      }
    }
    if (std::holds_alternative<session_event::Open>(event) || std::holds_alternative<session_event::Close>(event) ||
        std::holds_alternative<session_event::Show>(event) || std::holds_alternative<session_event::Rename>(event) ||
        std::holds_alternative<session_event::Cwd>(event) || std::holds_alternative<session_event::Kind>(event) ||
        std::holds_alternative<session_event::Join>(event) || std::holds_alternative<session_event::Leave>(event) ||
        std::holds_alternative<session_event::Swap>(event) || std::holds_alternative<session_event::Move>(event)) {
      m_sessionsChanged(save);
    }
    if (result == ApplyResult::LastClosed) {
      m_close();
      return;
    }
    if (regroup) {
      m_rebuildPending = true;
      m_window.requestRebuild();
    } else if (const auto* open = std::get_if<session_event::Open>(&event); open != nullptr && m_rowList != nullptr) {
      // A new session joins no split: it is the list's last child. Rows that animate out stay children until their
      // animation ends but have already left m_rows, so a child index is no m_rows index.
      m_window.updateInPlace([this, id = open->id](Renderer& /*renderer*/) {
        auto row = makeRow(id);
        m_rows.emplace_back(id, row.get());
        m_rowList->insertAnimated(std::move(row), m_rowList->children().size() - 1,
                                  Tokens::sizeRowTwoCompact * m_window.uiScale());
      });
    }
    if (const auto* close = std::get_if<session_event::Close>(&event); close != nullptr && m_rowList != nullptr && !regroup) {
      if (const auto it = std::ranges::find(m_rows, close->id, &std::pair<std::uint64_t, StatusRow*>::first);
          it != m_rows.end()) {
        StatusRow* row = it->second;
        m_rows.erase(it);
        m_rowList->removeAnimated(row, [this] { m_window.input().forgetDetached(); });
      }
    }
    if (m_sessions.shown() != before || m_sessions.visible() != m_builtPanes) {
      show(*m_sessions.shown());
    }
    refreshRows();
    m_window.invalidate();
  }

  void TerminalWindow::show(std::uint64_t id) {
    const std::vector<std::uint64_t> panes = m_sessions.visible();
    const bool rebuild = panes != m_builtPanes;
    if (m_findOpen && (rebuild || (m_split.split() && m_split.focusedPane() != id))) {
      closeFind();
    }
    if (std::holds_alternative<PaletteOpen>(m_palette) && (rebuild || (m_split.split() && m_split.focusedPane() != id))) {
      closePalette();
    }
    syncAgentBar();
    for (Session& session : m_views) {
      session.view->setWindowActive(std::ranges::contains(panes, session.id) && m_active);
    }
    syncTitle();
    if (!rebuild) {
      m_split.setFocused(id, true);
      return;
    }
    // Split and single differ in the window's frame (header or not): the window rebuilds and fills the slot.
    if (const bool split = panes.size() > 1; split != m_window.contentCards()) {
      m_window.setContentCards(split);
      return;
    }
    if (m_contentSlot != nullptr && !m_rebuildPending) {
      m_window.updateInPlace([this](Renderer& renderer) {
        for (Session& session : m_views) {
          session.view->sceneDestroyed();
        }
        std::vector<std::unique_ptr<Node>> removed;
        while (!m_contentSlot->children().empty()) {
          removed.push_back(m_contentSlot->removeChild(m_contentSlot->children().back().get()));
        }
        m_window.input().forgetDetached(); // while the removed areas still live
        removed.clear();
        fillSlot(renderer);
        m_contentSlot->markLayoutDirty();
      });
    }
  }

  void TerminalWindow::fillSlot(Renderer& renderer) {
    m_rebuildPending = false;
    m_captionLeads.clear();
    ContentView* view = shownView();
    if (view == nullptr) {
      m_builtPanes.clear();
      m_split.fill(renderer, m_contentSlot, m_contentWidth, m_contentHeight, {}, 0, false);
      return;
    }
    m_window.setMainFill(view->surface());
    m_window.setSceneContentBacked(dynamic_cast<TerminalView*>(view) != nullptr);
    // Before the window's frame follows (a rebuild is pending), the shown session fills the slot alone.
    const auto panes = m_sessions.visible();
    const bool cards = panes.size() > 1 && m_window.contentCards();
    m_builtPanes = cards ? panes : std::vector<std::uint64_t>{*m_sessions.shown()};
    m_split.fill(renderer, m_contentSlot, m_contentWidth, m_contentHeight, panes, *m_sessions.shown(), cards);
    if (cards) {
      refreshCards();
      m_split.fitCaptions(renderer);
    }
    // After this build is in the scene: the dispatcher focuses only attached areas.
    DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
      if (alive.expired()) {
        return;
      }
      m_retired.clear();
      focusGrid();
    });
  }

  Settings TerminalWindow::viewSettings() const {
    Settings settings = m_settings.settings();
    settings.fontSize = std::clamp(settings.fontSize + m_zoom, SettingsLimits::kFontSizeMin, SettingsLimits::kFontSizeMax);
    if (const auto* open = std::get_if<PaletteOpen>(&m_palette); open != nullptr && open->preview) {
      settings.theme = *open->preview;
    }
    return settings;
  }

  void TerminalWindow::applyViewSettings() {
    const Settings settings = viewSettings();
    for (Session& session : m_views) {
      if (auto* terminal = dynamic_cast<TerminalView*>(session.view.get())) {
        terminal->applySettings(settings, resolveTheme(settings.theme, m_settings.themes()));
      }
    }
  }

  bool TerminalWindow::sessionKey(const KeyboardEvent& event) {
    const auto action = event.pressed ? m_settings.settings().keys.match(event.sym, event.unshifted, event.modifiers)
                                      : std::nullopt;
    return action && runAction(*action);
  }

  bool TerminalWindow::runAction(Action action) {
    const auto& recent = m_sessions.recent();
    const auto step = [this](bool forward) {
      const auto order = sidebarOrder();
      const auto shown = m_sessions.shown();
      if (const auto it = std::ranges::find(order, shown.value_or(0)); order.size() > 1 && it != order.end()) {
        const auto by = static_cast<std::ptrdiff_t>(forward ? 1 : order.size() - 1);
        const auto index = static_cast<std::size_t>((it - order.begin() + by) % static_cast<std::ptrdiff_t>(order.size()));
        apply(session_event::Show{order[index]});
      }
    };
    // Zoom changes this window's text size only; the setting stays the size of new windows.
    const auto zoom = [this](int steps) {
      const int base = m_settings.settings().fontSize;
      m_zoom = std::clamp(steps, SettingsLimits::kFontSizeMin - base, SettingsLimits::kFontSizeMax - base);
      applyViewSettings();
    };
    switch (action) {
      case Action::NextRecent:
      case Action::PreviousRecent:
        if (recent.size() > 1) {
          apply(session_event::Show{action == Action::PreviousRecent ? recent.back() : recent[1]});
        }
        return true;
      case Action::SearchSessions:
        if (m_search != nullptr && m_window.sidebarVisible()) {
          m_window.input().setFocus(m_search->inputArea());
        }
        return true;
      case Action::PreviousSession: step(false); return true;
      case Action::NextSession: step(true); return true;
      case Action::FontBigger: zoom(m_zoom + 1); return true;
      case Action::FontSmaller: zoom(m_zoom - 1); return true;
      case Action::FontReset: zoom(0); return true;
      case Action::NewSession: openSession(NewShell{}); return true;
      case Action::SecureEntry: toggleSecureEntry(); return true;
      case Action::ShowScrollback:
        if (const auto shown = m_sessions.shown()) {
          showScrollback(*shown);
        }
        return true;
      case Action::NewAgent: openAgentHere(defaultAgentKind()); return true;
      // Closes the shown session, shell or agent; asks first while a command runs.
      case Action::CloseSession: askClose({*m_sessions.shown()}); return true;
      case Action::Preferences: m_preferences(m_loop.wayland().requestActivationToken(m_window.wlSurface())); return true;
      case Action::NewWindow: m_newWindow(m_loop.wayland().requestActivationToken(m_window.wlSurface()), {NewShell{}}); return true;
      case Action::Find: openFind(); return true;
      case Action::CommandPalette: openPalette(); return true;
      case Action::ToggleSidebar: m_window.setSidebarVisible(!m_window.sidebarVisible()); return true;
      case Action::Session1:
      case Action::Session2:
      case Action::Session3:
      case Action::Session4:
      case Action::Session5:
      case Action::Session6:
      case Action::Session7:
      case Action::Session8:
      case Action::Session9: {
        const auto index = static_cast<std::size_t>(action) - static_cast<std::size_t>(Action::Session1);
        if (const auto order = sidebarOrder(); index < order.size()) {
          apply(session_event::Show{order[index]});
        }
        return true;
      }
      // Without a split the keys go to the program.
      case Action::FocusLeft: return focusPane(-1.0F, 0.0F);
      case Action::FocusRight: return focusPane(1.0F, 0.0F);
      case Action::FocusUp: return focusPane(0.0F, -1.0F);
      case Action::FocusDown: return focusPane(0.0F, 1.0F);
      case Action::MovePaneLeft: return movePane(-1.0F, 0.0F);
      case Action::MovePaneRight: return movePane(1.0F, 0.0F);
      case Action::MovePaneUp: return movePane(0.0F, -1.0F);
      case Action::MovePaneDown: return movePane(0.0F, 1.0F);
      case Action::SplitRight: splitNew(andromeda::tiling::EdgeRight); return true;
      case Action::SplitDown: splitNew(andromeda::tiling::EdgeBottom); return true;
      default: return false; // the grid's own actions (TerminalView::key)
    }
  }

  std::vector<std::uint64_t> TerminalWindow::sidebarOrder() const {
    std::vector<std::uint64_t> order;
    for (const SessionInfo& session : m_sessions.sessions()) {
      if (std::ranges::contains(order, session.id)) {
        continue;
      }
      if (const auto* split = m_sessions.splitOf(session.id); split != nullptr) {
        order.insert(order.end(), split->begin(), split->end());
      } else {
        order.push_back(session.id);
      }
    }
    return order;
  }

  std::unique_ptr<StatusRow> TerminalWindow::makeRow(std::uint64_t id) {
    auto row = std::make_unique<StatusRow>(m_window.uiScale());
    row->setRail(m_rail);
    row->setOnClick([this, id] {
      if ((m_loop.wayland().keyboardModifiers() & KeyMod::Ctrl) != 0U) {
        ctrlClickRow(id);
        return;
      }
      apply(session_event::Show{id});
      focusGrid();
    });
    row->setOnSecondaryClick([this, id](float x, float y) { openRowMenu(id, x, y); });
    row->setOnDrag([this, id](StatusRow::DragPhase phase, float x, float y) { rowDrag(id, phase, x, y); });
    std::vector<StatusRow::Action> actions;
    if (m_sessions.splitOf(id) != nullptr) {
      actions.push_back({.glyph = "arrow-up-right", .tooltip = "Leave split", .onClick = [this, id] { apply(session_event::Leave{id}); }});
    }
    actions.push_back({.glyph = "close", .tooltip = "Close", .onClick = [this, id] { askClose({id}); }});
    row->setActions(std::move(actions));
    return row;
  }

  bool TerminalWindow::canJoinShown(std::uint64_t id) const {
    const std::uint64_t shown = m_sessions.shown().value_or(0);
    const auto* shownSplit = m_sessions.splitOf(shown);
    return id != shown && m_sessions.splitOf(id) == nullptr && (shownSplit == nullptr || shownSplit->size() < kMaxPanes);
  }

  void TerminalWindow::joinShown(std::uint64_t id) {
    const std::uint64_t shown = m_sessions.shown().value_or(0);
    m_split.pendJoin({.target = shown, .joining = id});
    apply(session_event::Join{.target = shown, .joining = id});
    focusGrid();
  }

  // Ctrl+click builds the shown split row by row: a free row joins it, a member leaves it; else a plain click.
  void TerminalWindow::ctrlClickRow(std::uint64_t id) {
    const std::uint64_t shown = m_sessions.shown().value_or(0);
    const auto* shownSplit = m_sessions.splitOf(shown);
    if (id != shown && shownSplit != nullptr && std::ranges::contains(*shownSplit, id)) {
      apply(session_event::Leave{id});
    } else if (canJoinShown(id)) {
      joinShown(id);
    } else {
      apply(session_event::Show{id});
      focusGrid();
    }
  }

  void TerminalWindow::openRowMenu(std::uint64_t id, float sceneX, float sceneY) {
    const Keybindings& keys = m_settings.settings().keys;
    enum : std::int32_t {
      kSplit = 1, kLeave, kRename, kDuplicate, kMoveToWindow, kShowScrollback, kSaveScrollback, kSecureEntry, kCloseOthers, kClose
    };
    const bool others = m_sessions.sessions().size() > 1;
    const bool member = m_sessions.splitOf(id) != nullptr;
    const bool canSplit = canJoinShown(id);
    std::vector<ContextMenuControlEntry> entries{{.id = kSplit, .label = "Split with current", .enabled = canSplit}};
    if (member) {
      entries.push_back({.id = kLeave, .label = "Leave split"});
    }
    entries.push_back({.id = kRename, .label = "Rename", .enabled = !m_rail});
    entries.push_back({.id = kDuplicate, .label = "Duplicate"});
    entries.push_back({.id = kMoveToWindow, .label = "Move to new window", .enabled = others});
    entries.push_back({.separator = true});
    const bool terminal = terminalOf(id) != nullptr;
    entries.push_back({.id = kShowScrollback, .label = "Show scrollback", .shortcut = keys.label(Action::ShowScrollback), .enabled = terminal});
    entries.push_back({.id = kSaveScrollback, .label = "Save scrollback…", .enabled = terminal});
    entries.push_back({.separator = true});
    entries.push_back({.id = kSecureEntry, .label = "Secure keyboard entry", .shortcut = keys.label(Action::SecureEntry),
                       .checkmark = true, .toggleState = m_secureEntry ? 1 : 0});
    entries.push_back({.separator = true});
    entries.push_back({.id = kCloseOthers, .label = "Close other sessions", .enabled = others});
    entries.push_back({.id = kClose, .label = "Close", .shortcut = keys.label(Action::CloseSession), .danger = true});
    m_window.openContextMenu(std::move(entries), sceneX, sceneY, [this, id](std::int32_t item) {
                               switch (item) {
                               case kSplit: joinShown(id); break;
                               case kLeave: apply(session_event::Leave{id}); break;
                               case kRename: renameSession(id); break;
                               case kDuplicate: duplicateSession(id); break;
                               case kMoveToWindow: moveToNewWindow(id); break;
                               case kShowScrollback: showScrollback(id); break;
                               case kSaveScrollback: saveScrollback(id); break;
                               case kSecureEntry: toggleSecureEntry(); break;
                               case kCloseOthers: {
                                 std::vector<std::uint64_t> ids;
                                 for (const SessionInfo& session : m_sessions.sessions()) {
                                   if (session.id != id) {
                                     ids.push_back(session.id);
                                   }
                                 }
                                 askClose(std::move(ids));
                                 break;
                               }
                               case kClose: askClose({id}); break;
                               default: break;
                               }
                             });
  }

  void TerminalWindow::renameSession(std::uint64_t id) {
    const auto it = std::ranges::find(m_rows, id, &std::pair<std::uint64_t, StatusRow*>::first);
    const SessionInfo* session = m_sessions.find(id);
    if (it == m_rows.end() || session == nullptr) {
      return;
    }
    // An empty name gives the row its automatic name (cwd or agent title) back.
    it->second->beginRename(rowData(*session).name, [this, id](std::optional<std::string> name) {
      if (name) {
        apply(session_event::Rename{id, std::move(*name)});
      }
      focusGrid();
    });
    m_window.input().setFocus(it->second->renameArea());
    m_window.invalidate();
  }

  TerminalView* TerminalWindow::terminalOf(std::uint64_t id) {
    const auto it = std::ranges::find(m_views, id, &Session::id);
    return it != m_views.end() ? it->view->terminal() : nullptr;
  }

  void TerminalWindow::focusSession(std::uint64_t id) {
    apply(session_event::Show{id});
    m_window.activate(m_loop.wayland().requestActivationToken(m_window.wlSurface()));
  }

  void TerminalWindow::closeSession(std::uint64_t id) { apply(session_event::Close{id}); }

  void TerminalWindow::renameSession(std::uint64_t id, std::string name) {
    apply(session_event::Rename{id, std::move(name)});
  }

  void TerminalWindow::showScrollback(std::uint64_t id) {
    TerminalView* view = terminalOf(id);
    if (view == nullptr) {
      return;
    }
    const std::string text = view->scrollbackText();
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    std::string path = std::string(runtime != nullptr && *runtime != '\0' ? runtime : "/tmp") + "/andromeda-scrollback-XXXXXX";
    const int fd = mkstemp(path.data()); // 0600
    if (fd < 0) {
      kLog.warn("scrollback file: {}", std::strerror(errno));
      return;
    }
    for (std::size_t done = 0; done < text.size();) {
      const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        break;
      }
      done += static_cast<std::size_t>(n);
    }
    ::close(fd);
    // The shell removes the copy when the pager quits or the session closes (SIGHUP).
    openSession(NewShell{.cwd = view->cwd(),
                         .argv = {"sh", "-c", "trap 'rm -f \"$1\"' EXIT HUP TERM; ${PAGER:-less +G} \"$1\"", "sh", path}});
  }

  void TerminalWindow::saveScrollback(std::uint64_t id) {
    TerminalView* view = terminalOf(id);
    if (view == nullptr || !m_chooseSaveFile) {
      return;
    }
    m_chooseSaveFile("scrollback.txt", [text = view->scrollbackText()](std::optional<std::filesystem::path> path) {
      if (!path) {
        return;
      }
      std::ofstream out(*path, std::ios::binary | std::ios::trunc);
      out << text;
      if (!out) {
        kLog.warn("cannot save scrollback to {}", path->string());
      }
    });
  }

  void TerminalWindow::duplicateSession(std::uint64_t id) {
    const SessionInfo* session = m_sessions.find(id);
    if (session == nullptr) {
      return;
    }
    if (session->kind == SessionKind::Agent) {
      const auto it = std::ranges::find(m_views, id, &Session::id);
      const AgentView* agent = it != m_views.end() ? it->view->agent() : nullptr;
      openSession(NewAgent{.kind = agent != nullptr ? agent->kind() : defaultAgentKind(), .cwd = session->cwd});
    } else {
      const TerminalView* view = terminalOf(id); // the row's cwd is the shown form (~/…), not a path
      openSession(NewShell{view != nullptr ? view->cwd() : std::string()});
    }
  }

  void TerminalWindow::askClose(std::vector<std::uint64_t> ids) {
    const auto close = [this](const std::vector<std::uint64_t>& targets) {
      for (const std::uint64_t target : targets) {
        apply(session_event::Close{target});
      }
    };
    const auto running = std::ranges::count_if(ids, [this](std::uint64_t target) {
      const SessionInfo* session = m_sessions.find(target);
      return session != nullptr && std::holds_alternative<CommandRunning>(session->command);
    });
    if (running == 0) {
      close(ids);
      return;
    }
    m_window.presentSheet(
        [this, ids, running, close](float s) {
          auto box = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP12 * s, .padding = Tokens::spaceP20 * s,
                                 .width = Tokens::componentTerminalFindWidth * s});
          const bool one = ids.size() == 1;
          box->addChild(ui::label({.text = one ? std::string("Close this session?") : std::format("Close {} sessions?", ids.size()),
                                   .fontSize = Tokens::typeTitle3.size * s, .fontWeight = FontWeight::SemiBold,
                                   .color = colorSpecFromRole(ColorRole::Label1)}));
          box->addChild(ui::label({.text = running == 1 ? std::string("A command is still running. Closing stops it.")
                                                        : std::format("Commands still run in {} sessions. Closing stops them.", running),
                                   .fontSize = Tokens::typeBody.size * s, .color = colorSpecFromRole(ColorRole::Label2),
                                   .maxLines = 3}));
          auto buttons = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Tokens::spaceP8 * s});
          buttons->addChild(ui::button({.text = std::string("Cancel"), .fontSize = Tokens::typeBody.size * s,
                                        .variant = ButtonVariant::Default, .onClick = [this] { m_window.dismissSheet(); }}));
          buttons->addChild(ui::button({.text = std::string("Close"), .fontSize = Tokens::typeBody.size * s,
                                        .variant = ButtonVariant::Destructive, .onClick = [this, ids, close] {
                                          m_window.dismissSheet();
                                          close(ids);
                                        }}));
          box->addChild(std::move(buttons));
          return box;
        },
        [this] { m_window.dismissSheet(); });
  }

  void TerminalWindow::askPaste(const std::string& text, std::function<void()> paste) {
    const auto lines = std::ranges::count(text, '\n') + (text.ends_with('\n') ? 0 : 1);
    std::string preview = text.substr(0, 400);
    m_window.presentSheet(
        [this, lines, preview, paste](float s) {
          auto box = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP12 * s, .padding = Tokens::spaceP20 * s,
                                 .width = Tokens::componentTerminalFindWidth * s});
          box->addChild(ui::label({.text = lines == 1 ? std::string("Paste and run this line?")
                                                      : std::format("Paste {} lines?", lines),
                                   .fontSize = Tokens::typeTitle3.size * s, .fontWeight = FontWeight::SemiBold,
                                   .color = colorSpecFromRole(ColorRole::Label1)}));
          box->addChild(ui::label({.text = std::string("The program does not guard pastes, so each line runs as a command."),
                                   .fontSize = Tokens::typeBody.size * s, .color = colorSpecFromRole(ColorRole::Label2),
                                   .maxLines = 3}));
          box->addChild(ui::label({.text = preview, .fontSize = Tokens::typeFootnote.size * s, .fontFamily = std::string(Tokens::fontMono),
                                   .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 6}));
          auto buttons = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Tokens::spaceP8 * s});
          buttons->addChild(ui::button({.text = std::string("Cancel"), .fontSize = Tokens::typeBody.size * s,
                                        .variant = ButtonVariant::Default, .onClick = [this] { m_window.dismissSheet(); }}));
          buttons->addChild(ui::button({.text = std::string("Paste"), .fontSize = Tokens::typeBody.size * s,
                                        .variant = ButtonVariant::Destructive, .onClick = [this, paste] {
                                          m_window.dismissSheet();
                                          paste();
                                        }}));
          box->addChild(std::move(buttons));
          return box;
        },
        [this] { m_window.dismissSheet(); });
  }

  void TerminalWindow::syncTitle() {
    const auto id = m_sessions.shown();
    const SessionInfo* session = id ? m_sessions.find(*id) : nullptr;
    if (session == nullptr) {
      return;
    }
    if (!session->name.empty() || !session->title.empty()) {
      m_window.setTitle(session->name.empty() ? session->title : session->name);
    } else if (session->kind == SessionKind::Agent) {
      ContentView* view = shownView();
      const agent::Session* model = view != nullptr && view->agent() != nullptr ? view->agent()->session() : nullptr;
      m_window.setTitle(model != nullptr && !model->title().empty() ? model->title() : std::string("New session"));
    } else if (!session->cwd.empty()) {
      m_window.setTitle(session->cwd);
    }
  }

  // The agent works in the shown session's directory; the sidebar keeps it with ~ for $HOME.
  agent::Kind TerminalWindow::defaultAgentKind() {
    if (ContentView* shown = shownView(); shown != nullptr && shown->agent() != nullptr) {
      return shown->agent()->kind();
    }
    const auto kinds = agent::kinds();
    const auto it = std::ranges::find_if(kinds, [](const agent::KindInfo& k) { return agent::installed(k.kind); });
    return it != kinds.end() ? it->kind : agent::Kind::Claude;
  }

  void TerminalWindow::openAgentHere(agent::Kind kind) {
    const SessionInfo* shown = m_sessions.shown() ? m_sessions.find(*m_sessions.shown()) : nullptr;
    std::string cwd = shown != nullptr ? shown->cwd : std::string();
    if (cwd.starts_with("~")) {
      cwd = homeDir() + cwd.substr(1);
    }
    openSession(NewAgent{.kind = kind, .cwd = cwd});
  }

  void TerminalWindow::openNewMenu() {
    if (m_newButton == nullptr) {
      return;
    }
    const Keybindings& keys = m_settings.settings().keys;
    enum : std::int32_t { kShell = 1, kWindow, kAgent }; // kAgent + Kind: one item per installed kind
    float x = 0.0F;
    float y = 0.0F;
    Node::absolutePosition(m_newButton, x, y);
    const agent::Kind shortcutKind = defaultAgentKind();
    std::vector<ContextMenuControlEntry> items{{.id = kShell, .label = "New session", .shortcut = keys.label(Action::NewSession), .glyph = "prompt"}};
    for (const agent::KindInfo& kind : agent::kinds()) {
      if (agent::installed(kind.kind) || kind.kind == shortcutKind) {
        items.push_back({.id = kAgent + static_cast<std::int32_t>(kind.kind), .label = "New " + std::string(kind.name) + " session",
                         .shortcut = kind.kind == shortcutKind ? keys.label(Action::NewAgent) : std::string(), .glyph = "robot"});
      }
    }
    items.push_back({.id = kWindow, .label = "New window", .shortcut = keys.label(Action::NewWindow), .glyph = "app-window"});
    m_window.openContextMenu(std::move(items),
                             x, y + m_newButton->height(),
                             [this, alive = std::weak_ptr<bool>(m_alive)](std::int32_t id) {
                               if (alive.expired()) {
                                 return;
                               }
                               switch (id) {
                                 case kShell: openSession(NewShell{}); break;
                                 case kWindow: m_newWindow(m_loop.wayland().requestActivationToken(m_window.wlSurface()), {NewShell{}}); break;
                                 default:
                                   if (id >= kAgent && id < kAgent + static_cast<std::int32_t>(agent::kKindCount)) {
                                     openAgentHere(static_cast<agent::Kind>(id - kAgent));
                                   }
                                   break;
                               }
                             });
  }

  // The shown agent session's pills fill the title band while find is closed; other sessions leave it empty.
  void TerminalWindow::syncAgentBar() {
    if (m_agentBar == nullptr) {
      return;
    }
    // An agent session's title is its first prompt (or the agent's session title).
    syncTitle();
    m_window.updateInPlace([this](Renderer& /*renderer*/) {
      fillAgentBar();
      m_agentBar->markLayoutDirty();
    });
  }

  void TerminalWindow::fillAgentBar() {
    while (!m_agentBar->children().empty()) {
      m_agentBar->removeChild(m_agentBar->children().back().get());
    }
    m_bandStatus = nullptr;
    m_agentBar->setParticipatesInLayout(!m_findOpen); // the find bar takes the whole slot
    ContentView* shown = shownView();
    AgentView* view = shown != nullptr ? shown->agent() : nullptr;
    if (view == nullptr || m_findOpen) {
      m_agentTick.stop();
      return;
    }
    if (!view->ticking()) {
      m_agentTick.stop();
    } else if (!m_agentTick.active()) {
      m_agentTick.startRepeating(std::chrono::seconds(1), [this] {
        ContentView* now = shownView();
        if (AgentView* agent = now != nullptr ? now->agent() : nullptr; agent != nullptr) {
          if (m_bandStatus != nullptr) {
            m_bandStatus->setText(agent->statusText());
          }
          agent->tick();
          m_window.invalidate();
        }
      });
    }
    const std::uint64_t key = view->key();
    m_agentBar->addChild(view->band(m_window.uiScale(), [this, key](const Node* anchor) {
      if (AgentView* agent = agentView(key); agent != nullptr) {
        m_window.openPopover(anchor, 360.0F, agent->planHeight(), [this, key](float scale) -> std::unique_ptr<Node> {
          AgentView* open = agentView(key);
          return open != nullptr ? open->planList(scale) : ui::column({});
        });
      }
    }, &m_bandStatus));
  }

  AgentView* TerminalWindow::agentView(std::uint64_t key) {
    for (Session& session : m_views) {
      if (AgentView* view = session.view->agent(); view != nullptr && view->key() == key) {
        return view;
      }
    }
    return nullptr;
  }

  void TerminalWindow::agentChanged(std::uint64_t key) {
    for (Session& session : m_views) {
      if (AgentView* view = session.view->agent(); view != nullptr && view->key() == key) {
        view->changed();
        if (m_sessions.shown() == session.id) {
          syncAgentBar();
        }
        refreshRows();
        m_window.invalidate();
        return;
      }
    }
  }

  StatusRow::Data TerminalWindow::rowData(const SessionInfo& session) const {
    const std::string& place = session.host.empty() ? session.cwd : session.host;
    StatusRow::Data data{.name = session.name.empty() ? place : session.name,
                         .unseen = session.seen == Seen::Unseen,
                         .selected = session.id == m_rowSelected};
    if (session.kind == SessionKind::Agent) {
      const auto it = std::ranges::find(m_views, session.id, &Session::id);
      const agent::Session* model = it != m_views.end() && it->view->agent() != nullptr ? it->view->agent()->session() : nullptr;
      const agent::RowStatus status = model != nullptr ? model->rowStatus() : agent::RowStatus::Failed;
      data.symbol = "robot";
      switch (status) {
        case agent::RowStatus::Starting: data.status = "starting"; break;
        case agent::RowStatus::Ready: data.status = "ready"; break;
        case agent::RowStatus::Working:
          data.spinning = true;
          data.status = it->view->agent()->activity();
          break;
        case agent::RowStatus::NeedsPermission:
          data.symbol = "hand-stop";
          data.status = "needs permission";
          break;
        case agent::RowStatus::Done: data.status = "done"; break;
        case agent::RowStatus::Declined:
          data.symbol = "ban";
          data.status = "declined";
          break;
        case agent::RowStatus::Limited: data.status = "stopped at a limit"; break;
        case agent::RowStatus::Stopped: data.status = "stopped by you"; break;
        case agent::RowStatus::Failed:
          data.symbol = "alert-circle";
          data.symbolDanger = true;
          data.status = "failed";
          if (model != nullptr) {
            const auto& entries = model->entries();
            const auto stop = std::find_if(entries.rbegin(), entries.rend(),
                                           [](const agent::Entry& e) { return std::holds_alternative<agent::Stop>(e); });
            if (stop != entries.rend() && std::get<agent::Stop>(*stop).exitCode) {
              data.status += " \u00b7 agent exited " + std::to_string(*std::get<agent::Stop>(*stop).exitCode);
            }
          }
          break;
      }
      if (model != nullptr && !model->title().empty()) {
        data.name = model->title();
      }
      return data;
    }
    // The ssh or container command runs for the whole remote session: the row names the place, not that command.
    if (session.kind == SessionKind::Ssh || session.kind == SessionKind::Container) {
      data.symbol = session.kind == SessionKind::Ssh ? "server" : "box";
      data.status = session.kind == SessionKind::Ssh ? "ssh" : "container";
    } else if (const auto* running = std::get_if<CommandRunning>(&session.command)) {
      // A quiet program (an editor, a pager, a sleep) is not loading work: no spinner and no frames (rulebook P21).
      // A shown session's grid already shows its output, so only sessions off screen spin.
      data.spinning = running->output == CommandOutput::Active && !std::ranges::contains(m_sessions.visible(), session.id);
      data.symbol = data.spinning ? "" : "player-play";
      data.status = withCommand("running", running->command);
    } else if (const auto* finished = std::get_if<CommandFinished>(&session.command)) {
      data.symbol = finished->failed() ? "alert-circle" : "circle-check";
      data.symbolDanger = finished->failed();
      // A success the user has seen gives line 2 back to the program title; a failure stays until the next command.
      if (finished->failed() || session.seen == Seen::Unseen || session.title.empty()) {
        data.status =
            withCommand(finished->failed() ? "exit " + std::to_string(*finished->exitCode) : "done", finished->command);
      }
    } else {
      switch (session.kind) {
        case SessionKind::Shell: data.symbol = "prompt"; break;
        case SessionKind::App: data.symbol = "app-window"; break;
        case SessionKind::Ssh:
        case SessionKind::Container: break;
        case SessionKind::Agent: data.symbol = "robot"; break;
      }
    }
    if (session.kind == SessionKind::App) {
      data.symbol = "app-window";
    }
    // Line 2 is status first (rulebook); a program title fills it only when no status shows.
    if (data.status.empty() && session.title != data.name) {
      data.status = session.title;
    }
    return data;
  }

  // Rows change in place.
  void TerminalWindow::refreshRows() {
    if (const std::uint64_t shown = m_sessions.shown().value_or(0); m_rowList != nullptr && shown != m_tintTarget) {
      m_tintTarget = shown;
      const auto it = std::ranges::find(m_rows, shown, &std::pair<std::uint64_t, StatusRow*>::first);
      // A split member's tint is its group's block.
      const auto* split = m_sessions.splitOf(shown);
      const auto group = split != nullptr ? std::ranges::find(m_groups, split->front(), &std::pair<std::uint64_t, Flex*>::first)
                                          : m_groups.end();
      Node* target = group != m_groups.end() ? static_cast<Node*>(group->second)
                     : it != m_rows.end()     ? static_cast<Node*>(it->second)
                                              : nullptr;
      // The label weight moves to the new row at half the tint's travel.
      m_rowList->setSelected(target, true, [this, shown] {
        m_rowSelected = shown;
        refreshRows();
        m_window.invalidate();
      });
    }
    const std::uint64_t focused = m_sessions.shown().value_or(0);
    for (const auto& [id, row] : m_rows) {
      const SessionInfo* session = m_sessions.find(id);
      if (session == nullptr) {
        continue;
      }
      StatusRow::Data data = rowData(*session);
      // A split member's row is one line; the shown split's focused member is a raised chip on the group's tint.
      if (m_sessions.splitOf(id) != nullptr) {
        data.status.clear();
        if (const auto view = std::ranges::find(m_views, id, &Session::id); id == focused && view != m_views.end()) {
          data.chip = view->view->surface();
        }
      }
      row->setData(data);
    }
    const SessionInfo* shown = m_sessions.shown() ? m_sessions.find(*m_sessions.shown()) : nullptr;
    StatusRow::Data data = shown != nullptr ? rowData(*shown) : StatusRow::Data{};
    const std::string state = shown != nullptr ? captionState(*shown, data.status) : std::string{};
    if (shown != nullptr) {
      lockSymbol(shown->id, data);
    }
    m_window.setCaption({.symbol = data.symbol, .state = state, .danger = data.symbolDanger, .spinning = data.spinning});
    if (refreshCards()) {
      m_window.updateInPlace([this](Renderer& renderer) {
        m_split.fitCaptions(renderer);
        m_contentSlot->markLayoutDirty();
      });
    }
    syncTitle();
  }

  bool TerminalWindow::matches(const SessionInfo& session) const {
    const auto lower = [](std::string text) {
      std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return text;
    };
    const std::string needle = lower(m_filter);
    return needle.empty() || lower(session.name).find(needle) != std::string::npos ||
           lower(session.cwd).find(needle) != std::string::npos;
  }

  void TerminalWindow::setFilter(const std::string& text) {
    m_filter = text;
    applyFilter();
    m_window.invalidate();
  }

  // In place, not by rebuild: a rebuild would take the keys from the search field.
  void TerminalWindow::applyFilter() {
    for (const auto& [id, row] : m_rows) {
      const SessionInfo* session = m_sessions.find(id);
      const bool visible = session != nullptr && matches(*session);
      row->setVisible(visible);
      row->setParticipatesInLayout(visible);
    }
    if (m_rowList != nullptr) {
      m_rowList->markLayoutDirty();
    }
  }

  void TerminalWindow::openFirstMatch() {
    for (const SessionInfo& session : m_sessions.sessions()) {
      if (matches(session)) {
        apply(session_event::Show{session.id});
        break;
      }
    }
    focusGrid();
  }

  void TerminalWindow::focusGrid() {
    if (ContentView* view = shownView(); view != nullptr && view->focusArea() != nullptr) {
      m_window.input().setFocus(view->focusArea());
    }
  }

  void TerminalWindow::openFind() {
    if (m_findBar == nullptr || m_findInput == nullptr || shownTerminal() == nullptr) {
      return;
    }
    m_findOpen = true;
    m_findBar->setVisible(true);
    m_findBar->setParticipatesInLayout(true);
    m_window.setHeaderSearchMode(true);
    syncAgentBar();
    m_window.input().setFocus(m_findInput->inputArea());
    if (TerminalView* view = shownTerminal(); view != nullptr && !m_findQuery.empty()) {
      view->setFindQuery(m_findQuery, m_findOptions);
    }
  }

  void TerminalWindow::toggleFindOption(bool regex) {
    bool& option = regex ? m_findOptions.regex : m_findOptions.matchCase;
    option = !option;
    if (Button* button = m_findToggles[regex ? 1 : 0]; button != nullptr) {
      button->setSelected(option);
    }
    if (TerminalView* view = shownTerminal(); view != nullptr && !m_findQuery.empty()) {
      view->setFindQuery(m_findQuery, m_findOptions);
    }
  }

  void TerminalWindow::closeFind() {
    m_findOpen = false;
    for (Session& session : m_views) {
      if (TerminalView* terminal = session.view->terminal(); terminal != nullptr) {
        terminal->closeFind();
      }
    }
    if (m_findBar != nullptr) {
      m_findBar->setVisible(false);
      m_findBar->setParticipatesInLayout(false);
    }
    m_window.setHeaderSearchMode(false);
    syncAgentBar();
    focusGrid();
  }

  std::vector<palette::Item> TerminalWindow::paletteItems(PaletteTarget target,
                                                          std::unordered_map<std::string, std::function<void()>>& runs) {
    std::vector<palette::Item> items;
    const std::uint64_t shown = m_sessions.shown().value_or(0);
    const auto sessionItem = [&](TerminalWindow& window, std::uint64_t id, std::string trailing) {
      const SessionInfo* info = window.m_sessions.find(id);
      const StatusRow::Data data = window.rowData(*info);
      items.push_back({.kind = palette::Kind::Session, .id = std::format("session:{}", id), .label = data.name,
                       .sub = data.status, .trailing = std::move(trailing),
                       .glyph = data.spinning ? std::string("loader") : data.symbol, .danger = data.symbolDanger});
      return items.back().id;
    };
    if (target == PaletteTarget::SplitWith) {
      for (const std::uint64_t id : sidebarOrder()) {
        if (canJoinShown(id)) {
          runs[sessionItem(*this, id, {})] = [this, id] { joinShown(id); };
        }
      }
      return items;
    }
    const std::vector<TerminalWindow*> windows = m_windows ? m_windows() : std::vector<TerminalWindow*>{this};
    for (std::size_t n = 0; n < windows.size(); ++n) {
      TerminalWindow* window = windows[n];
      for (const std::uint64_t id : window->sidebarOrder()) {
        std::string trailing = window != this ? std::format("window {}", n + 1) : id == shown ? "this pane" : "";
        runs[sessionItem(*window, id, std::move(trailing))] = [window, id] { window->focusSession(id); };
      }
    }
    const Keybindings& keys = m_settings.settings().keys;
    const auto addAction = [&](std::string id, std::string label, std::vector<std::string> caps, std::string off,
                               std::function<void()> run) {
      items.push_back({.kind = palette::Kind::Action, .id = id, .label = std::move(label), .keys = std::move(caps),
                       .off = std::move(off), .glyph = "player-play"});
      runs[std::move(id)] = std::move(run);
    };
    constexpr std::string_view kCapSeparator = "\x1f";
    for (const ActionInfo& info : actionTable()) {
      const Action action = info.action;
      if (action == Action::CommandPalette ||
          (static_cast<int>(action) >= static_cast<int>(Action::Session1) &&
           static_cast<int>(action) <= static_cast<int>(Action::Session9))) {
        continue;
      }
      std::vector<std::string> caps;
      for (const auto part : std::views::split(keys.label(action, kCapSeparator), kCapSeparator)) {
        caps.emplace_back(std::string_view(part));
      }
      const bool focus = action == Action::FocusLeft || action == Action::FocusRight || action == Action::FocusUp ||
                         action == Action::FocusDown;
      std::string off;
      if (focus && !m_split.split()) {
        off = "Not in a split";
      } else if (action == Action::Copy && (shownTerminal() == nullptr || !shownTerminal()->hasSelection())) {
        off = "No selection";
      }
      addAction(std::format("action:{}", info.name), std::string(info.label), std::move(caps), std::move(off), [this, action] {
                  if (TerminalView* view = shownTerminal(); view != nullptr && view->runAction(action)) {
                    return;
                  }
                  runAction(action);
                });
    }
    const bool others = m_sessions.sessions().size() > 1;
    const bool canSplit = std::ranges::any_of(sidebarOrder(), [this](std::uint64_t id) { return canJoinShown(id); });
    addAction("menu:split", "Split with…", {}, canSplit ? "" : "No session can join",
              [this] { openPalette(PaletteTarget::SplitWith); });
    addAction("menu:leave", "Leave split", {}, m_sessions.splitOf(shown) != nullptr ? "" : "Not in a split",
              [this, shown] { apply(session_event::Leave{shown}); });
    addAction("menu:rename", "Rename…", {}, m_rail ? "Sidebar is a rail" : "", [this, shown] { renameSession(shown); });
    addAction("menu:duplicate", "Duplicate session", {}, "", [this, shown] { duplicateSession(shown); });
    addAction("menu:move", "Move to new window", {}, others ? "" : "Only one session",
              [this, shown] { moveToNewWindow(shown); });
    addAction("menu:save", "Save scrollback…", {}, terminalOf(shown) != nullptr ? "" : "Not a terminal",
              [this, shown] { saveScrollback(shown); });
    addAction("menu:close-others", "Close other sessions", {}, others ? "" : "Only one session", [this, shown] {
      std::vector<std::uint64_t> ids;
      for (const SessionInfo& session : m_sessions.sessions()) {
        if (session.id != shown) {
          ids.push_back(session.id);
        }
      }
      askClose(std::move(ids));
    });
    const auto pack = [](const Color& c) {
      const auto byte = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F)); };
      return (byte(c.r) << 24U) | (byte(c.g) << 16U) | (byte(c.b) << 8U) | byte(c.a);
    };
    const std::string current = m_settings.settings().theme;
    for (const ThemeChoice& choice : themeChoices(m_settings.themes())) {
      const ThemePalette colours = themePalette(choice.spec);
      std::string id = "theme:" + choice.id;
      items.push_back({.kind = palette::Kind::Theme, .id = id, .label = choice.name,
                       .trailing = choice.id == current ? "current" : "",
                       .swatch = {pack(colours.view), pack(colours.accent), pack(colours.ansi[3])}});
      runs[std::move(id)] = [this, theme = choice.id] {
        Settings settings = m_settings.settings();
        settings.theme = theme;
        m_settings.update(settings);
      };
    }
    return items;
  }

  void TerminalWindow::openPalette(PaletteTarget target) {
    if (std::holds_alternative<PaletteOpen>(m_palette)) {
      closePalette();
      return;
    }
    if (m_contentSlot == nullptr || shownView() == nullptr) {
      return;
    }
    if (m_findOpen) {
      closeFind();
    }
    PaletteOpen open;
    std::vector<palette::Item> items = paletteItems(target, open.runs);
    const bool any = target == PaletteTarget::Any;
    open.ui = std::make_shared<CommandPalette>(
        m_window.animations(), std::move(items), any && m_loadRecent ? m_loadRecent() : std::vector<std::string>{},
        CommandPalette::Host{
            .run =
                [this, any](const palette::Item& item) {
                  auto* now = std::get_if<PaletteOpen>(&m_palette);
                  if (now == nullptr) {
                    return;
                  }
                  const auto it = now->runs.find(item.id);
                  if (it == now->runs.end()) {
                    return;
                  }
                  std::function<void()> run = std::move(it->second);
                  // Session ids are per run: a saved one could name another session after a restart.
                  if (any && item.kind != palette::Kind::Session && m_loadRecent && m_saveRecent) {
                    std::vector<std::string> recent = m_loadRecent();
                    std::erase(recent, item.id);
                    recent.insert(recent.begin(), item.id);
                    recent.resize(std::min<std::size_t>(recent.size(), 10));
                    m_saveRecent(std::move(recent));
                  }
                  closePalette();
                  run();
                },
            .runNoMatch =
                [this](const std::string& query) {
                  closePalette();
                  const std::uint64_t id = openSession(NewShell{});
                  apply(session_event::Rename{id, query});
                },
            .current =
                [this](const palette::Item* item) {
                  auto* now = std::get_if<PaletteOpen>(&m_palette);
                  if (now == nullptr) {
                    return;
                  }
                  std::optional<std::string> preview;
                  if (item != nullptr && item->kind == palette::Kind::Theme) {
                    preview = item->id.substr(std::string_view("theme:").size());
                  }
                  if (preview != now->preview) {
                    now->preview = std::move(preview);
                    applyViewSettings();
                  }
                },
            .close = [this] { closePalette(); },
            .relayout =
                [this] {
                  m_window.updateInPlace([this](Renderer& renderer) {
                    if (auto* now = std::get_if<PaletteOpen>(&m_palette); now != nullptr) {
                      now->root->layout(renderer);
                      now->ui->afterLayout(true);
                    }
                  });
                  m_window.invalidate();
                },
        });
    const float s = m_window.uiScale();
    const CommandPalette::Rect slot{0.0F, 0.0F, m_contentWidth, m_contentHeight};
    CommandPalette::Rect dim = slot;
    CommandPalette::Rect dock = slot;
    const std::uint64_t pane = m_split.focusedPane() != 0 ? m_split.focusedPane() : m_sessions.shown().value_or(0);
    if (const auto card = m_split.rect(pane)) {
      const PaneRect& r = *card;
      const float caption = Tokens::componentWindowCaption * s;
      dim = {r.x, r.y + caption, r.width, r.height - caption};
      if (r.width >= Tokens::componentTerminalPaletteNarrow * s) {
        dock = {r.x, r.y, r.width, r.height};
      }
    }
    m_palette = std::move(open);
    m_window.updateInPlace([&](Renderer& renderer) {
      auto& now = std::get<PaletteOpen>(m_palette);
      auto root = now.ui->build(s, slot.width, slot.height, dim, dock);
      now.root = m_contentSlot->addChild(std::move(root));
      now.root->layout(renderer);
      now.ui->afterLayout(false);
      now.ui->grow();
    });
    m_window.input().setFocus(std::get<PaletteOpen>(m_palette).ui->focusArea());
    m_window.invalidate();
  }

  void TerminalWindow::closePalette(bool rootGone) {
    auto* open = std::get_if<PaletteOpen>(&m_palette);
    if (open == nullptr) {
      return;
    }
    PaletteOpen closing = std::move(*open);
    m_palette = PaletteClosed{};
    if (closing.preview) {
      applyViewSettings();
    }
    if (rootGone) {
      // Its nodes leave with the old slot later: they must not reach a freed palette.
      closing.ui->detach();
      DeferredCall::callLater([ui = closing.ui] {});
      return;
    }
    const bool focused = m_window.input().focusedArea() == closing.ui->focusArea();
    closing.ui->shrink([this, ui = closing.ui, root = closing.root, alive = std::weak_ptr<bool>(m_alive)] {
      // Later: the root's destructor cancels the animation that runs this.
      DeferredCall::callLater([this, ui, root, alive] {
        if (alive.expired() || m_contentSlot == nullptr) {
          return;
        }
        for (const auto& child : m_contentSlot->children()) {
          if (child.get() == root) {
            auto removed = m_contentSlot->removeChild(root);
            m_window.input().forgetDetached();
            break;
          }
        }
        m_window.invalidate();
      });
    });
    if (focused) {
      focusGrid();
    }
  }

  void TerminalWindow::setFindCount(std::size_t total, std::optional<std::size_t> current, FindError error) {
    if (m_findCount == nullptr) {
      return;
    }
    m_findCount->setText(m_findQuery.empty()                    ? std::string()
                         : error == FindError::InvalidPattern ? std::string("Invalid pattern")
                         : error == FindError::TooComplex     ? std::string("Pattern too complex")
                         : total == 0                         ? std::string("No matches")
                         : current          ? std::format("{} of {}", *current, total)
                                            : std::format("{} matches", total));
    for (Button* step : m_findSteps) {
      if (step != nullptr) {
        step->setEnabled(total > 0);
      }
    }
  }

  void TerminalWindow::registerDropTarget() {
    wl_surface* surface = m_window.wlSurface();
    DragAndDrop* dnd = m_loop.wayland().dragAndDrop();
    if (surface == nullptr || dnd == nullptr || surface == m_dropSurface) {
      return;
    }
    m_dropSurface = surface;
    dnd->setAcceptedTypes({"text/uri-list", "text/plain;charset=utf-8", "text/plain"});
    const auto over = [this](float x, float y) {
      TerminalView* view = shownTerminal();
      return view != nullptr && view->focusArea() != nullptr && view->focusArea()->containsScenePoint(x, y) ? view : nullptr;
    };
    dnd->setTarget(surface, {
        .enter = [](const std::vector<std::string>& /*mimes*/) { return true; },
        .motion = [over](float x, float y) -> std::uint32_t { return over(x, y) != nullptr ? DragAndDrop::Copy : DragAndDrop::None; },
        .leave = [] {},
        .drop =
            [over](float x, float y, std::uint32_t /*action*/, const std::string& mime, std::string data) {
              if (TerminalView* view = over(x, y); view != nullptr) {
                view->pasteDrop(mime, data);
              }
            },
    });
  }

  TerminalWindow::~TerminalWindow() {
    m_alive.reset();
    if (DragAndDrop* dnd = m_loop.wayland().dragAndDrop(); dnd != nullptr && m_dropSurface != nullptr) {
      dnd->clearTarget(m_dropSurface);
    }
    if (m_tickSurface != nullptr && m_tickSurface == m_window.surface()) {
      m_tickSurface->setFrameTickCallback(nullptr);
      m_tickSurface->setScaleChangedCallback(nullptr);
    }
  }

  void TerminalWindow::surfaceLost() {
    m_tickSurface = nullptr;
    m_dropSurface = nullptr; // the old surface died with its target
    m_bandStatus = nullptr;
    for (Session& session : m_views) {
      session.view->sceneDestroyed();
    }
  }

  void TerminalWindow::openUri(const std::string& uri) {
    // The opened app activates its window with this token; without one the compositor only marks it urgent.
    std::unique_ptr<GAppLaunchContext, void (*)(gpointer)> context(g_app_launch_context_new(), g_object_unref);
    if (const std::string token = m_loop.wayland().requestActivationToken(m_window.wlSurface()); !token.empty()) {
      g_app_launch_context_setenv(context.get(), "XDG_ACTIVATION_TOKEN", token.c_str());
    }
    GError* error = nullptr;
    if (!g_app_info_launch_default_for_uri(uri.c_str(), context.get(), &error)) {
      kLog.warn("open {}: {}", uri, error != nullptr ? error->message : "?");
      g_clear_error(&error);
    }
  }

  SplitView::Callbacks TerminalWindow::splitCallbacks() {
    return {
        .splitOf = [this](std::uint64_t id) { return m_sessions.splitOf(id); },
        .content =
            [this](Renderer& renderer, std::uint64_t id, float width, float height) -> std::unique_ptr<Node> {
              const auto it = std::ranges::find(m_views, id, &Session::id);
              if (it == m_views.end()) {
                return std::make_unique<Node>();
              }
              Surface* surface = m_window.surface();
              const float deviceScale = surface != nullptr ? surface->effectiveBufferScale() : 1.0F;
              return it->view->build(renderer, m_window.uiScale(), deviceScale, width, height);
            },
        .caption =
            [this](std::uint64_t id) {
              const float s = m_window.uiScale();
              CaptionLead& lead = m_captionLeads[id];
              auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
              row->addChild(ui::glyph({.out = &lead.symbol, .glyphSize = Tokens::sizeSymbol * s}));
              row->addChild(ui::spinner({.out = &lead.spinner, .color = colorSpecFromRole(ColorRole::Label2),
                                         .spinnerSize = Tokens::sizeSymbol * s, .visible = false,
                                         .participatesInLayout = false}));
              const ColorSpec clear = clearColorSpec();
              Button::ButtonPalette ghost;
              ghost.normal = {.bg = clear, .border = clear, .label = colorSpecFromRole(ColorRole::Label2)};
              ghost.hover = {.bg = colorSpecFromRole(ColorRole::Fill1), .border = clear,
                             .label = colorSpecFromRole(ColorRole::Label1)};
              ghost.pressed = {.bg = colorSpecFromRole(ColorRole::Fill2), .border = clear, .label = ghost.hover.label};
              ghost.disabled = ghost.normal;
              Button* menu = nullptr;
              auto button = ui::button({.out = &menu,
                                        .glyph = std::string("dots"),
                                        .glyphSize = Tokens::spaceP12 * s,
                                        .customPalette = ghost,
                                        .tooltip = std::string("Session menu"),
                                        .radius = Tokens::spaceP24 * s / 2.0F,
                                        .width = Tokens::spaceP24 * s,
                                        .height = Tokens::spaceP24 * s});
              menu->setOnClick([this, menu, id] {
                float x = 0.0F;
                float y = 0.0F;
                Node::absolutePosition(menu, x, y);
                openRowMenu(id, x, y + menu->height());
              });
              return SplitView::Caption{.lead = std::move(row), .trail = std::move(button)};
            },
        .surface =
            [this](std::uint64_t id) {
              const auto it = std::ranges::find(m_views, id, &Session::id);
              return it != m_views.end() ? it->view->surface() : ColorSpec{};
            },
        .focus =
            [this](std::uint64_t id) {
              apply(session_event::Show{id});
              focusGrid();
            },
        .minimum = [this] { return paneMinimum(); },
        .settle =
            [this](std::uint64_t id, float width, float height) {
              if (const auto it = std::ranges::find(m_views, id, &Session::id); it != m_views.end()) {
                it->view->setSize(width, height);
              }
            },
        .ring = [this] { return m_settings.settings().focusRing; },
    };
  }

  std::pair<float, float> TerminalWindow::paneMinimum() const {
    const float s = m_window.uiScale();
    float cellW = 0.0F;
    float cellH = 0.0F;
    for (const Session& session : m_views) {
      if (const TerminalView* view = session.view->terminal(); view != nullptr && view->cellWidth() > 0.0F) {
        cellW = view->cellWidth();
        cellH = view->cellHeight();
        break;
      }
    }
    return {Tokens::componentTerminalSplitMinColumns * cellW + 2.0F * Tokens::spaceP16 * s,
            Tokens::componentTerminalSplitMinRows * cellH + Tokens::componentWindowCaption * s +
                (Tokens::spaceP16 + Tokens::spaceP12) * s};
  }

  std::optional<ui::SplitDrop> TerminalWindow::dropTarget(std::uint64_t dragged, float sceneX, float sceneY) const {
    const auto shown = m_sessions.shown();
    if (!shown || dragged == *shown || m_sessions.splitOf(dragged) != nullptr) {
      return std::nullopt;
    }
    if (const auto* split = m_sessions.splitOf(*shown); split != nullptr && split->size() >= kMaxPanes) {
      return std::nullopt;
    }
    return m_split.dropAt(*shown, sceneX, sceneY);
  }

  // A member dropped outside its sidebar block leaves the split.
  bool TerminalWindow::leavesGroup(std::uint64_t id, float sceneX, float sceneY) const {
    const auto* split = m_sessions.splitOf(id);
    if (split == nullptr) {
      return false;
    }
    const auto group = std::ranges::find(m_groups, split->front(), &std::pair<std::uint64_t, Flex*>::first);
    return group != m_groups.end() && !group->second->containsScenePoint(sceneX, sceneY);
  }

  std::optional<std::uint64_t> TerminalWindow::blockOf(const Node* child) const {
    if (!child->visible()) {
      return std::nullopt;
    }
    const auto node = [](const auto& entry) { return static_cast<const Node*>(entry.second); };
    if (const auto row = std::ranges::find(m_rows, child, node); row != m_rows.end()) {
      return row->first;
    }
    if (const auto group = std::ranges::find(m_groups, child, node); group != m_groups.end()) {
      return group->first;
    }
    return std::nullopt;
  }

  std::optional<std::uint64_t> TerminalWindow::dropBefore(std::uint64_t dragged, float sceneX, float sceneY) const {
    if (m_rowList == nullptr || m_rail) {
      return std::nullopt;
    }
    float x = 0.0F;
    float y = 0.0F;
    Node::absolutePosition(m_rowList, x, y);
    if (sceneX < x || sceneX > x + m_rowList->width()) {
      return std::nullopt;
    }
    for (const auto& child : m_rowList->children()) {
      const auto block = blockOf(child.get());
      if (!block) {
        continue;
      }
      Node::absolutePosition(child.get(), x, y);
      if (sceneY >= y + child->height() / 2.0F) {
        continue;
      }
      // A group sits in list order where its earliest member is.
      const auto* split = m_sessions.splitOf(*block);
      const auto lead = std::ranges::find_if(m_sessions.sessions(), [&](const SessionInfo& session) {
        return session.id == *block || (split != nullptr && std::ranges::contains(*split, session.id));
      });
      return lead->id == dragged ? std::nullopt : std::optional(lead->id);
    }
    return std::uint64_t{0};
  }

  void TerminalWindow::showDropLine(std::optional<std::uint64_t> before) {
    if (m_dropLine == nullptr || m_rowList == nullptr) {
      return;
    }
    m_dropLine->setVisible(before.has_value());
    if (!before) {
      return;
    }
    const float s = m_window.uiScale();
    const float line = Tokens::componentTerminalSidebarDropLine * s;
    // In the gap above the block it lands before, or under the last block.
    float y = m_rowList->height();
    for (const auto& child : m_rowList->children()) {
      const auto block = blockOf(child.get());
      const auto* split = block ? m_sessions.splitOf(*block) : nullptr;
      if (block && (*block == *before || (split != nullptr && std::ranges::contains(*split, *before)))) {
        y = child->y() - Tokens::spaceP4 * s / 2.0F;
        break;
      }
    }
    float sceneX = 0.0F;
    float sceneY = 0.0F;
    Node::mapToScene(m_rowList, 0.0F, y, sceneX, sceneY);
    float localX = 0.0F;
    float localY = 0.0F;
    (void)Node::mapFromScene(m_dropLine->parent(), sceneX, sceneY, localX, localY);
    m_dropLine->setPosition(localX, localY - line / 2.0F);
    m_dropLine->setSize(m_rowList->width(), line);
  }

  void TerminalWindow::rowDrag(std::uint64_t id, StatusRow::DragPhase phase, float sceneX, float sceneY) {
    const auto target = dropTarget(id, sceneX, sceneY);
    // A raised chip of the row under the pointer, its symbol where the pointer holds it.
    const float s = m_window.uiScale();
    if (phase == StatusRow::DragPhase::Move && !m_dragGhost) {
      const auto row = std::ranges::find(m_rows, id, &std::pair<std::uint64_t, StatusRow*>::first);
      const auto view = std::ranges::find(m_views, id, &Session::id);
      if (const SessionInfo* session = m_sessions.find(id); session != nullptr && row != m_rows.end() && view != m_views.end()) {
        auto ghost = std::make_unique<StatusRow>(s);
        StatusRow::Data data = rowData(*session);
        data.chip = view->view->surface();
        ghost->setData(data);
        ghost->setMinWidth(row->second->width());
        ghost->setMaxWidth(row->second->width());
        m_window.setDragGhost(std::move(ghost));
        m_dragGhost = true;
      }
    }
    if (phase == StatusRow::DragPhase::Move) {
      m_window.moveDragGhost(sceneX - Tokens::spaceP16 * s, sceneY - Tokens::sizeRowTwoCompact * s / 2.0F);
    } else if (std::exchange(m_dragGhost, false)) {
      m_window.clearDragGhost();
    }
    m_split.showDropPreview(target && phase == StatusRow::DragPhase::Move ? std::optional(target->rect) : std::nullopt);
    // Off a pane, the row lands in the sidebar order; a split member only once it leaves its block.
    const bool leaves = !target && leavesGroup(id, sceneX, sceneY);
    const auto lands = target || (m_sessions.splitOf(id) != nullptr && !leaves) ? std::nullopt : dropBefore(id, sceneX, sceneY);
    showDropLine(phase == StatusRow::DragPhase::Move ? lands : std::nullopt);
    if (target && phase == StatusRow::DragPhase::Drop) {
      m_split.pendJoin({.target = target->target, .joining = id, .edge = target->edge}, target->rect);
      apply(session_event::Join{.target = target->target, .joining = id, .before = target->before});
      focusGrid();
    } else if (phase == StatusRow::DragPhase::Drop) {
      if (leaves) {
        apply(session_event::Leave{id});
      }
      if (lands) {
        apply(session_event::Move{.id = id, .before = *lands});
      }
    }
  }

  bool TerminalWindow::refreshCards() {
    bool changed = false;
    for (const std::uint64_t id : m_builtPanes) {
      const SessionInfo* session = m_sessions.find(id);
      const auto lead = m_captionLeads.find(id);
      if (session == nullptr || lead == m_captionLeads.end()) {
        continue;
      }
      StatusRow::Data data = rowData(*session);
      data.status = captionState(*session, data.status);
      lockSymbol(id, data);
      const ColorSpec ink = colorSpecFromRole(data.symbolDanger ? ColorRole::Danger : ColorRole::Label2);
      changed = m_split.setCaption(id, data.name, data.status, ink) || changed;
      Glyph* symbolGlyph = lead->second.symbol;
      Spinner* spinner = lead->second.spinner;
      const bool symbol = !data.spinning && GlyphRegistry::contains(data.symbol);
      changed = changed || symbolGlyph->visible() != symbol;
      if (symbol && symbolGlyph->setGlyph(data.symbol)) {
        changed = true;
      }
      symbolGlyph->setColor(ink);
      symbolGlyph->setVisible(symbol);
      symbolGlyph->setParticipatesInLayout(symbol);
      spinner->setVisible(data.spinning);
      spinner->setParticipatesInLayout(data.spinning);
      if (data.spinning != spinner->spinning()) {
        data.spinning ? spinner->start() : spinner->stop();
      }
    }
    return changed;
  }

  bool TerminalWindow::focusPane(float dx, float dy) {
    if (!m_split.split()) {
      return false;
    }
    if (const auto best = m_split.paneInDirection(dx, dy)) {
      apply(session_event::Show{*best});
      focusGrid();
    }
    return true;
  }

  bool TerminalWindow::movePane(float dx, float dy) {
    const auto shown = m_sessions.shown();
    if (!m_split.split() || !shown) {
      return false;
    }
    if (const auto other = m_split.paneInDirection(dx, dy)) {
      m_split.swapPanes(*shown, *other);
      apply(session_event::Swap{*shown, *other});
    }
    return true;
  }

  void TerminalWindow::splitNew(std::uint32_t edge) {
    const auto target = m_sessions.shown();
    if (const auto* split = target ? m_sessions.splitOf(*target) : nullptr; !target || (split != nullptr && split->size() >= kMaxPanes)) {
      return;
    }
    const std::uint64_t id = openSession(NewShell{});
    m_split.pendJoin({.target = *target, .joining = id, .edge = edge});
    apply(session_event::Join{.target = *target, .joining = id, .before = edge == andromeda::tiling::EdgeLeft || edge == andromeda::tiling::EdgeTop});
    focusGrid();
  }

  void TerminalWindow::requestFrame() {
    m_window.noteSceneActivity(); // output, a blink or an animation: the scene moves with the terminal
    Surface* surface = m_window.surface();
    if (surface == nullptr) {
      return;
    }
    if (surface != m_tickSurface) {
      m_tickSurface = surface;
      surface->setFrameTickCallback([this](float) {
        m_loop.renderContext().makeCurrent(m_window.surface()->renderTarget());
        // Hidden sessions run frame() too: it reports their exit; without a grid it draws nothing.
        for (std::size_t i = 0; i < m_views.size(); ++i) {
          m_views[i].view->frame();
        }
      });
      surface->setScaleChangedCallback([this](float scale) {
        for (Session& session : m_views) {
          session.view->setDeviceScale(scale);
        }
      });
    }
    surface->requestFrameTick();
  }

} // namespace term
