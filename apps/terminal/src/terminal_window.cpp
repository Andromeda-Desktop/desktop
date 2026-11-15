#include "terminal_window.h"

#include "app/app_loop.h"
#include "app/app_window.h"
#include "notifier.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/selection_list.h"
#include "ui/controls/status_row.h"
#include "ui/controls/flex.h"
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

    std::string loginShell() {
      if (const char* shell = std::getenv("SHELL"); shell != nullptr && *shell != '\0') {
        return shell;
      }
      if (const passwd* pw = getpwuid(getuid()); pw != nullptr && pw->pw_shell != nullptr) {
        return pw->pw_shell;
      }
      return "/bin/sh";
    }

    // The status comes first, so truncation removes the command, never the outcome.
    std::string withCommand(const std::string& status, const std::string& command) {
      return command.empty() ? status : status + " · " + command;
    }

    std::string homeDir() {
      const char* home = std::getenv("HOME");
      return home != nullptr ? home : "/";
    }
  } // namespace

  TerminalWindow::TerminalWindow(AppLoop& loop, AppWindow& window, Notifier& notifier,
                                 const std::vector<WindowOrigin>& origins, std::function<void()> close)
      : m_loop(loop), m_window(window), m_notifier(notifier), m_close(std::move(close)) {
    // The content is a slot: a session switch swaps the view inside it in place, so the sidebar keeps its nodes.
    m_window.setContentBuilder([this](Renderer& renderer, float width, float height) -> std::unique_ptr<Node> {
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
      auto top = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
      top->addChild(ui::searchCapsule({
          .out = &m_search,
          .placeholder = "Sessions",
          .keycap = "Ctrl K",
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
      top->addChild(ui::button({.glyph = std::string("plus"), .glyphSize = Tokens::sizeSymbol * s,
                                .variant = ButtonVariant::Ghost, .tooltip = std::string("New session  Ctrl+Shift+T"),
                                .radius = button / 2.0F, .height = button,
                                .onClick = [this] { openSession(NewShell{}); }}));
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
    m_window.setFirstKeyHandler([this](const KeyboardEvent& event) {
      if (sessionKey(event)) {
        return true;
      }
      TerminalView* view = shownView();
      return view != nullptr && view->focusArea() != nullptr && m_window.input().focusedArea() == view->focusArea() &&
             view->key(event);
    });
    m_window.setActiveCallback([this](bool active) {
      m_active = active;
      if (TerminalView* view = shownView(); view != nullptr) {
        view->setWindowActive(active);
      }
    });
    m_window.setLayoutCallback([this] {
      const AppWindow::Rect rect = m_window.contentRect();
      if (TerminalView* view = shownView(); view != nullptr) {
        view->setSize(rect.width, rect.height);
      }
    });
    for (const WindowOrigin& origin : origins) {
      openSession(origin);
    }
  }

  void TerminalWindow::openSession(const WindowOrigin& origin) {
    const std::uint64_t id = m_nextId++;
    auto alive = std::weak_ptr<bool>(m_alive);
    auto view = std::make_unique<TerminalView>(TerminalView::Host{
        .requestFrame = [this] { requestFrame(); },
        .textures = [this]() -> TextureManager* {
          RenderContext& render = m_loop.renderContext();
          return render.glReady() ? &render.textureManager() : nullptr;
        },
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
              const std::string home = homeDir();
              const bool inHome = path == home || path.starts_with(home + "/");
              const std::string shown = inHome ? "~" + path.substr(home.size()) : path;
              apply(session_event::Cwd{id, shown});
              if (m_sessions.shown() == id) {
                m_window.setTitle(shown);
              }
            },
        .openUri = [this](const std::string& uri) { openUri(uri); }});
    TerminalView& ref = *view;
    m_views.push_back({id, std::move(view)});
    const auto* reattach = std::get_if<Reattach>(&origin);
    if (reattach == nullptr || !ref.reattach(reattach->heldId)) {
      const auto* fresh = std::get_if<NewShell>(&origin);
      const std::string cwd = fresh != nullptr ? fresh->cwd : std::string();
      const std::string shell = loginShell();
      if (!ref.start({shell, "-l"}, cwd.empty() ? homeDir() : cwd)) {
        kLog.warn("cannot start {}", shell);
      }
    }
    apply(session_event::Open{id, SessionKind::Shell, "", ""});
  }

  TerminalView* TerminalWindow::shownView() {
    const auto id = m_sessions.shown();
    if (!id) {
      return nullptr;
    }
    const auto it = std::ranges::find(m_views, *id, &Session::id);
    return it == m_views.end() ? nullptr : it->view.get();
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
    for (Session& session : m_views) {
      session.view->setWindowActive(session.id == id && m_active);
    }
    if (const SessionInfo* info = m_sessions.find(id); info != nullptr && !info->cwd.empty()) {
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
    TerminalView* view = shownView();
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
    row->setOnClick([this, id] {
      apply(session_event::Show{id});
      focusGrid();
    });
    return row;
  }

  StatusRow::Data TerminalWindow::rowData(const SessionInfo& session) const {
    StatusRow::Data data{.name = session.name.empty() ? session.cwd : session.name,
                         .unseen = session.seen == Seen::Unseen,
                         .selected = session.id == m_rowSelected};
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
    if (TerminalView* view = shownView(); view != nullptr && view->focusArea() != nullptr) {
      m_window.input().setFocus(view->focusArea());
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
