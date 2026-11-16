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
#include "ui/builders.h"
#include "ui/controls/selection_list.h"
#include "ui/controls/status_row.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"

#include <gio/gio.h>
#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace term {

  namespace {
    constexpr Logger kLog("terminal");

    // The status comes first, so truncation removes the command, never the outcome.
    std::string withCommand(const std::string& status, const std::string& command) {
      return command.empty() ? status : status + " · " + command;
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

  TerminalWindow::TerminalWindow(AppLoop& loop, AppWindow& window, Notifier& notifier, agent::Host& agents,
                                 ThumbnailService& thumbnails, SettingsStore& settings,
                                 const std::vector<WindowOrigin>& origins, std::function<void()> close,
                                 std::function<void(const std::string& token)> newWindow,
                                 std::function<void(const std::string& token)> preferences)
      : m_loop(loop), m_window(window), m_notifier(notifier), m_agents(agents), m_thumbnails(thumbnails), m_settings(settings), m_close(std::move(close)),
        m_newWindow(std::move(newWindow)), m_preferences(std::move(preferences)) {
    m_settingsChanged = m_settings.changed().connect([this] {
      for (Session& session : m_views) {
        if (auto* terminal = dynamic_cast<TerminalView*>(session.view.get())) {
          terminal->applySettings(m_settings.settings());
        }
      }
    });
    // The content is a slot: a session switch swaps the view inside it in place, so the sidebar keeps its nodes.
    m_window.setContentBuilder([this](Renderer& renderer, float width, float height) -> std::unique_ptr<Node> {
      m_bandStatus = nullptr; // the header rebuilds after the content and sets it again
      for (Session& session : m_views) {
        session.view->sceneDestroyed();
      }
      auto slot = ui::column({.align = FlexAlign::Stretch, .width = width, .height = height});
      m_contentSlot = slot.get();
      m_contentWidth = width;
      m_contentHeight = height;
      fillSlot(renderer);
      return slot;
    });
    m_window.setSidebarBuilder([this](Renderer& /*renderer*/, float width, float /*height*/) -> std::unique_ptr<Node> {
      const float s = m_window.uiScale();
      const float button = Tokens::sizeControlM * s;
      auto side = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s, .padding = Tokens::spaceP8 * s,
                              .width = width});
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
            .height = Tokens::sizeControlL,
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
      top->addChild(ui::button({.out = &m_newButton, .glyph = std::string("plus"), .glyphSize = Tokens::sizeSymbol * s,
                                .variant = ButtonVariant::Ghost, .tooltip = std::string("New"),
                                .radius = button / 2.0F, .height = button,
                                .onClick = [this] { openNewMenu(); }}));
      side->addChild(std::move(top));
      auto list = std::make_unique<SelectionList>(s);
      list->setGap(Tokens::spaceP4 * s);
      m_rows.clear();
      m_tintTarget = 0;
      for (const SessionInfo& session : m_sessions.sessions()) {
        auto row = makeRow(session.id);
        m_rows.push_back({session.id, row.get()});
        list->addChild(std::move(row));
      }
      m_rowList = list.get();
      side->addChild(std::move(list));
      refreshRows();
      applyFilter();
      return side;
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
                  view->setFindQuery(text);
                }
                if (text.empty()) {
                  setFindCount(0, std::nullopt);
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
            return false;
          },
      }));
      bar->addChild(std::move(field));
      bar->addChild(ui::label({.out = &m_findCount, .fontSize = Tokens::typeFootnote.size * s,
                               .fontFeatures = std::string("tnum"), .color = colorSpecFromRole(ColorRole::Label2),
                               .maxLines = 1}));
      const auto round = [&](const char* glyph, const char* tooltip, Button** out, std::function<void()> onClick) {
        return ui::button({.out = out, .glyph = std::string(glyph), .glyphSize = Tokens::spaceP12 * s,
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
      setFindCount(0, std::nullopt);
      return slot;
    });
    m_window.setHeaderAccessoryFloor([this](Renderer& renderer) { return m_agentBar->measure(renderer, {}).width; });
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
      if (ContentView* view = shownView(); view != nullptr) {
        view->setWindowActive(active);
      }
    });
    m_window.setLayoutCallback([this] {
      const AppWindow::Rect rect = m_window.contentRect();
      if (ContentView* view = shownView(); view != nullptr) {
        view->setSize(rect.width, rect.height);
      }
    });
    for (const WindowOrigin& origin : origins) {
      openSession(origin);
    }
  }

  void TerminalWindow::openSession(const WindowOrigin& origin) {
    const std::uint64_t id = m_nextId++;
    if (const auto* fresh = std::get_if<NewAgent>(&origin)) {
      const std::uint64_t key = s_nextAgentKey++;
      const std::string cwd = fresh->cwd.empty() ? homeDir() : fresh->cwd;
      m_agents.open(key, cwd);
      m_views.push_back({id, std::make_unique<AgentView>(m_agents, key)});
      apply(session_event::Open{id, SessionKind::Agent, "Claude", shownPath(cwd)});
      return;
    }
    auto alive = std::weak_ptr<bool>(m_alive);
    auto view = std::make_unique<TerminalView>(TerminalView::Host{
        .requestFrame = [this] { requestFrame(); },
        .playBell = [this] { m_loop.wayland().ringBell(m_window.wlSurface()); },
        .textures = [this]() -> TextureManager* {
          RenderContext& render = m_loop.renderContext();
          return render.glReady() ? &render.textureManager() : nullptr;
        },
        .thumbnails = &m_thumbnails,
        // exited runs inside the view's frame(): the view is destroyed later.
        .exited =
            [this, id, alive](int) {
              DeferredCall::callLater([this, id, alive] {
                if (!alive.expired()) {
                  apply(session_event::Close{id});
                }
              });
            },
        .modifiers = [this] { return m_loop.wayland().keyboardModifiers(); },
        .commandFinished =
            [this](const std::string& summary, const std::string& body) {
              m_notifier.notify(summary, body, [this, alive = std::weak_ptr<bool>(m_alive)](const std::string& token) {
                if (!alive.expired()) {
                  m_window.activate(token);
                }
              });
            },
        .commandStarted =
            [this, id](const std::string& command) {
              apply(session_event::CommandStart{id, command, SessionClock::now()});
            },
        .commandEnded =
            [this, id](std::optional<int> exitCode) {
              apply(session_event::CommandEnd{id, exitCode, SessionClock::now()});
            },
        .cwdChanged =
            [this, id](const std::string& path) {
              const std::string shown = shownPath(path);
              apply(session_event::Cwd{id, shown});
              if (m_sessions.shown() == id) {
                m_window.setTitle(shown);
              }
            },
        .openUri = [this](const std::string& uri) { openUri(uri); },
        .findChanged =
            [this, id](std::size_t total, std::optional<std::size_t> current) {
              if (m_sessions.shown() == id) {
                setFindCount(total, current);
              }
            },
        .settings = m_settings.settings(),
        .glass = m_window.material() == AppWindow::Material::Glass});
    TerminalView& ref = *view;
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
      const std::string shell = m_settings.settings().shell.empty() ? defaultShell() : m_settings.settings().shell;
      const ShellLaunch launch = shellLaunch(shell, shellIntegrationDir(), [](const char* name) { return std::getenv(name); });
      if (!ref.start(launch.argv, cwd.empty() ? homeDir() : cwd, launch.env)) {
        kLog.warn("cannot start {}", shell);
      }
    }
    apply(session_event::Open{id, SessionKind::Shell, "", ""});
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

  void TerminalWindow::apply(const SessionListEvent& event) {
    const auto before = m_sessions.shown();
    const ApplyResult result = m_sessions.apply(event);
    if (result == ApplyResult::Rejected) {
      return;
    }
    if (const auto* close = std::get_if<session_event::Close>(&event)) {
      const auto it = std::ranges::find(m_views, close->id, &Session::id);
      if (close->id == m_builtId) {
        m_retired.push_back(std::move(*it));
      }
      m_views.erase(it);
    }
    if (result == ApplyResult::LastClosed) {
      m_close();
      return;
    }
    if (const auto* open = std::get_if<session_event::Open>(&event); open != nullptr && m_rowList != nullptr) {
      const auto& sessions = m_sessions.sessions();
      const auto index = static_cast<std::size_t>(std::ranges::find(sessions, open->id, &SessionInfo::id) - sessions.begin());
      m_window.updateInPlace([this, id = open->id, index](Renderer& /*renderer*/) {
        auto row = makeRow(id);
        m_rows.insert(m_rows.begin() + static_cast<std::ptrdiff_t>(index), {id, row.get()});
        m_rowList->insertAnimated(std::move(row), index, Tokens::sizeRowTwoCompact * m_window.uiScale());
      });
    }
    if (const auto* close = std::get_if<session_event::Close>(&event); close != nullptr && m_rowList != nullptr) {
      if (const auto it = std::ranges::find(m_rows, close->id, &std::pair<std::uint64_t, StatusRow*>::first);
          it != m_rows.end()) {
        StatusRow* row = it->second;
        m_rows.erase(it);
        m_rowList->removeAnimated(row, [this] { m_window.input().forgetDetached(); });
      }
    }
    if (m_sessions.shown() != before) {
      show(*m_sessions.shown());
    }
    refreshRows();
    m_window.invalidate();
  }

  void TerminalWindow::show(std::uint64_t id) {
    if (m_findOpen && m_builtId != id) {
      closeFind();
    }
    syncAgentBar();
    for (Session& session : m_views) {
      session.view->setWindowActive(session.id == id && m_active);
    }
    if (const SessionInfo* info = m_sessions.find(id);
        info != nullptr && info->kind != SessionKind::Agent && !info->cwd.empty()) {
      m_window.setTitle(info->cwd);
    }
    if (id != m_builtId && m_contentSlot != nullptr) {
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
    ContentView* view = shownView();
    m_builtId = view != nullptr ? *m_sessions.shown() : 0;
    if (view == nullptr) {
      return;
    }
    Surface* surface = m_window.surface();
    const float deviceScale = surface != nullptr ? surface->effectiveBufferScale() : 1.0F;
    m_contentSlot->addChild(view->build(renderer, m_window.uiScale(), deviceScale, m_contentWidth, m_contentHeight));
    // After this build is in the scene: the dispatcher focuses only attached areas.
    DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
      if (alive.expired()) {
        return;
      }
      m_retired.clear();
      focusGrid();
    });
  }

  bool TerminalWindow::sessionKey(const KeyboardEvent& event) {
    constexpr std::uint32_t kTab = 0xff09;
    constexpr std::uint32_t kLeftTab = 0xfe20;
    const std::uint32_t mods = event.modifiers & (KeyMod::Ctrl | KeyMod::Shift | KeyMod::Alt | KeyMod::Super);
    if (!event.pressed || (mods & KeyMod::Ctrl) == 0 || (mods & (KeyMod::Alt | KeyMod::Super)) != 0) {
      return false;
    }
    const bool shift = (mods & KeyMod::Shift) != 0;
    const auto& recent = m_sessions.recent();
    if (event.sym == kTab || event.sym == kLeftTab) {
      if (recent.size() > 1) {
        apply(session_event::Show{shift ? recent.back() : recent[1]});
      }
      return true;
    }
    if (!shift && (event.unshifted == 'k' || event.unshifted == 'K')) {
      if (m_search != nullptr && m_window.sidebarVisible()) {
        m_window.input().setFocus(m_search->inputArea());
      }
      return true;
    }
    if (shift && (event.unshifted == 't' || event.unshifted == 'T')) {
      openSession(NewShell{});
      return true;
    }
    if (shift && (event.unshifted == 'a' || event.unshifted == 'A')) {
      openAgentHere();
      return true;
    }
    // A shell session ends with its shell; an agent session ends here.
    if (shift && (event.unshifted == 'w' || event.unshifted == 'W')) {
      if (ContentView* view = shownView(); view != nullptr && view->agent() != nullptr) {
        apply(session_event::Close{*m_sessions.shown()});
      }
      return true;
    }
    if (!shift && event.unshifted == ',') {
      m_preferences(m_loop.wayland().requestActivationToken(m_window.wlSurface()));
      return true;
    }
    if (shift && (event.unshifted == 'n' || event.unshifted == 'N')) {
      m_newWindow(m_loop.wayland().requestActivationToken(m_window.wlSurface()));
      return true;
    }
    if (shift && (event.unshifted == 'f' || event.unshifted == 'F')) {
      openFind();
      return true;
    }
    if (!shift && event.unshifted >= '1' && event.unshifted <= '9') {
      const auto index = static_cast<std::size_t>(event.unshifted - '1');
      if (index < m_sessions.sessions().size()) {
        apply(session_event::Show{m_sessions.sessions()[index].id});
      }
      return true;
    }
    return false;
  }

  std::unique_ptr<StatusRow> TerminalWindow::makeRow(std::uint64_t id) {
    auto row = std::make_unique<StatusRow>(m_window.uiScale());
    row->setRail(m_rail);
    row->setOnClick([this, id] {
      apply(session_event::Show{id});
      focusGrid();
    });
    return row;
  }

  // The agent works in the shown session's directory; the sidebar keeps it with ~ for $HOME.
  void TerminalWindow::openAgentHere() {
    const SessionInfo* shown = m_sessions.shown() ? m_sessions.find(*m_sessions.shown()) : nullptr;
    std::string cwd = shown != nullptr ? shown->cwd : std::string();
    if (cwd.starts_with("~")) {
      cwd = homeDir() + cwd.substr(1);
    }
    openSession(NewAgent{cwd});
  }

  void TerminalWindow::openNewMenu() {
    if (m_newButton == nullptr) {
      return;
    }
    enum : std::int32_t { kShell = 1, kAgent, kWindow, kPreferences };
    float x = 0.0F;
    float y = 0.0F;
    Node::absolutePosition(m_newButton, x, y);
    m_window.openContextMenu({{.id = kShell, .label = "New session", .shortcut = "Ctrl+Shift+T", .glyph = "prompt"},
                              {.id = kAgent, .label = "New agent", .shortcut = "Ctrl+Shift+A", .glyph = "robot"},
                              {.id = kWindow, .label = "New window", .shortcut = "Ctrl+Shift+N", .glyph = "app-window"},
                              {.id = kPreferences, .label = "Preferences", .shortcut = "Ctrl+,", .glyph = "settings"}},
                             x, y + m_newButton->height(),
                             [this, alive = std::weak_ptr<bool>(m_alive)](std::int32_t id) {
                               if (alive.expired()) {
                                 return;
                               }
                               switch (id) {
                                 case kShell: openSession(NewShell{}); break;
                                 case kAgent: openAgentHere(); break;
                                 case kWindow: m_newWindow(m_loop.wayland().requestActivationToken(m_window.wlSurface())); break;
                                 case kPreferences: m_preferences(m_loop.wayland().requestActivationToken(m_window.wlSurface())); break;
                                 default: break;
                               }
                             });
  }

  // The shown agent session's pills fill the title band while find is closed; other sessions leave it empty.
  void TerminalWindow::syncAgentBar() {
    if (m_agentBar == nullptr) {
      return;
    }
    // An agent session's title is its first prompt (or the agent's session title).
    if (ContentView* shown = shownView(); shown != nullptr && shown->agent() != nullptr) {
      const agent::Session* model = shown->agent()->session();
      m_window.setTitle(model != nullptr && !model->title().empty() ? model->title() : std::string("New session"));
    }
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
    StatusRow::Data data{.name = session.name.empty() ? session.cwd : session.name,
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
          data.symbol = "loader-2";
          data.status = it->view->agent()->activity();
          break;
        case agent::RowStatus::NeedsPermission:
          data.symbol = "hand-stop";
          data.status = "needs permission";
          break;
        case agent::RowStatus::Done: data.status = "done"; break;
        case agent::RowStatus::Stopped: data.status = "stopped by you"; break;
        case agent::RowStatus::Failed:
          data.symbol = "alert-circle";
          data.symbolDanger = true;
          data.status = "failed";
          break;
      }
      if (model != nullptr && !model->title().empty()) {
        data.name = model->title();
      }
      return data;
    }
    if (const auto* running = std::get_if<CommandRunning>(&session.command)) {
      data.symbol = "loader-2";
      data.status = withCommand("running", running->command);
      data.time = formatClockTime(
          std::chrono::duration_cast<std::chrono::seconds>(SessionClock::now() - running->start).count());
    } else if (const auto* finished = std::get_if<CommandFinished>(&session.command)) {
      data.symbol = finished->failed() ? "alert-circle" : "circle-check";
      data.symbolDanger = finished->failed();
      data.status =
          withCommand(finished->failed() ? "exit " + std::to_string(*finished->exitCode) : "done", finished->command);
      data.time = formatClockTime(std::chrono::duration_cast<std::chrono::seconds>(finished->duration).count());
    } else {
      switch (session.kind) {
        case SessionKind::Shell: data.symbol = "prompt"; break;
        case SessionKind::App: data.symbol = "app-window"; break;
        case SessionKind::Ssh: data.symbol = "server"; break;
        case SessionKind::Container: data.symbol = "box"; break;
        case SessionKind::Agent: data.symbol = "robot"; break;
      }
    }
    if (session.kind == SessionKind::App) {
      data.symbol = "app-window";
    }
    return data;
  }

  // Rows change in place; the 1 Hz tick runs only while a command runs, for its elapsed time.
  void TerminalWindow::refreshRows() {
    if (const std::uint64_t shown = m_sessions.shown().value_or(0); m_rowList != nullptr && shown != m_tintTarget) {
      m_tintTarget = shown;
      const auto it = std::ranges::find(m_rows, shown, &std::pair<std::uint64_t, StatusRow*>::first);
      // The label weight moves to the new row at half the tint's travel.
      m_rowList->setSelected(it != m_rows.end() ? it->second : nullptr, true, [this, shown] {
        m_rowSelected = shown;
        refreshRows();
        m_window.invalidate();
      });
    }
    bool running = false;
    for (const auto& [id, row] : m_rows) {
      if (const SessionInfo* session = m_sessions.find(id); session != nullptr) {
        row->setData(rowData(*session));
        running = running || std::holds_alternative<CommandRunning>(session->command);
      }
    }
    if (!running) {
      m_tick.stop();
    } else if (!m_tick.active()) {
      m_tick.startRepeating(std::chrono::seconds(1), [this] {
        refreshRows();
        m_window.invalidate();
      });
    }
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
      view->setFindQuery(m_findQuery);
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

  void TerminalWindow::setFindCount(std::size_t total, std::optional<std::size_t> current) {
    if (m_findCount == nullptr) {
      return;
    }
    m_findCount->setText(m_findQuery.empty() ? std::string()
                         : total == 0       ? std::string("No matches")
                         : current          ? std::format("{} of {}", *current, total)
                                            : std::format("{} matches", total));
    for (Button* step : m_findSteps) {
      if (step != nullptr) {
        step->setEnabled(total > 0);
      }
    }
  }

  TerminalWindow::~TerminalWindow() {
    m_alive.reset();
    if (m_tickSurface != nullptr && m_tickSurface == m_window.surface()) {
      m_tickSurface->setFrameTickCallback(nullptr);
      m_tickSurface->setScaleChangedCallback(nullptr);
    }
  }

  void TerminalWindow::surfaceLost() {
    m_tickSurface = nullptr;
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

  void TerminalWindow::requestFrame() {
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
