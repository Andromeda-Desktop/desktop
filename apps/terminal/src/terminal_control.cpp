#include "terminal_control.h"
#include "terminal_app.h"

#include "dbus/session_bus.h"

#include <algorithm>
#include <filesystem>
#include <format>

namespace term {

  namespace {

    std::string_view kindName(SessionKind kind) {
      switch (kind) {
      case SessionKind::Shell: return "shell";
      case SessionKind::App: return "app";
      case SessionKind::Ssh: return "ssh";
      case SessionKind::Container: return "container";
      case SessionKind::Agent: return "agent";
      }
      return "shell";
    }

    [[noreturn]] void fail(const char* name, const std::string& message) {
      throw sdbus::Error(sdbus::Error::Name{name}, message);
    }

    TerminalView::TextExtent extentOf(const std::string& name) {
      if (name == "screen") {
        return TerminalView::TextExtent::Screen;
      }
      if (name == "all") {
        return TerminalView::TextExtent::All;
      }
      if (name == "selection") {
        return TerminalView::TextExtent::Selection;
      }
      if (name == "last_output") {
        return TerminalView::TextExtent::LastOutput;
      }
      fail(control::kInvalidArgs, std::format("extent '{}': use screen, all, selection or last_output", name));
    }

  } // namespace

  TerminalWindow& TerminalApp::controlWindow(std::uint64_t id) {
    for (const auto& [window, terminal] : m_windows) {
      if (terminal->sessions().find(id) != nullptr) {
        return *terminal;
      }
    }
    fail(control::kNoSession, std::format("no session {}", id));
  }

  TerminalView& TerminalApp::controlTerminal(std::uint64_t id) {
    TerminalView* view = controlWindow(id).terminalOf(id);
    if (view == nullptr) {
      fail(control::kNotTerminal, std::format("session {} is an agent session", id));
    }
    return *view;
  }

  std::uint64_t TerminalApp::controlOpen(const std::map<std::string, sdbus::Variant>& options) {
    NewShell shell;
    std::string name;
    std::optional<std::uint32_t> windowId;
    for (const auto& [key, value] : options) {
      if (key == "cwd") {
        shell.cwd = value.get<std::string>();
      } else if (key == "argv") {
        shell.argv = value.get<std::vector<std::string>>();
      } else if (key == "name") {
        name = value.get<std::string>();
      } else if (key == "window") {
        windowId = value.get<std::uint32_t>();
      } else {
        fail(control::kInvalidArgs, std::format("unknown option '{}'", key));
      }
    }
    if (std::error_code error; !shell.cwd.empty() && !std::filesystem::is_directory(shell.cwd, error)) {
      fail(control::kInvalidArgs, std::format("cwd '{}' is no directory", shell.cwd));
    }
    // No window option: the newest window; 0: a new window.
    TerminalWindow* target = nullptr;
    for (const auto& [window, terminal] : m_windows) {
      const bool match = windowId ? terminal->windowId() == *windowId
                                  : target == nullptr || terminal->windowId() > target->windowId();
      if (match) {
        target = terminal.get();
      }
    }
    if (target == nullptr && windowId.value_or(0) != 0) {
      fail(control::kNoWindow, std::format("no window {}", *windowId));
    }
    std::uint64_t id = 0;
    if (target != nullptr) {
      id = target->openSession(shell);
    } else {
      m_nextOrigins = {std::move(shell)};
      AppWindow& window = createWindow();
      target = m_windows.at(&window).get();
      id = target->sessions().sessions().front().id;
      window.open();
    }
    if (!name.empty()) {
      target->renameSession(id, std::move(name));
    }
    return id;
  }

  void TerminalApp::exportControl() {
    if (bus() == nullptr) {
      return;
    }
    using Entry = std::map<std::string, sdbus::Variant>;
    m_control = sdbus::createObject(bus()->connection(), sdbus::ObjectPath{control::kPath});
    m_control
        ->addVTable(
            sdbus::registerMethod("ListSessions").withOutputParamNames("sessions").implementedAs([this] {
              std::vector<Entry> out;
              for (const auto& [window, terminal] : m_windows) {
                const SessionList& list = terminal->sessions();
                for (const SessionInfo& info : list.sessions()) {
                  const TerminalView* view = terminal->terminalOf(info.id);
                  std::string command;
                  if (const auto* running = std::get_if<CommandRunning>(&info.command)) {
                    command = running->command;
                  } else if (const auto* done = std::get_if<CommandFinished>(&info.command)) {
                    command = done->command;
                  }
                  const auto* split = list.splitOf(info.id);
                  out.push_back(Entry{
                      {"id", sdbus::Variant{info.id}},
                      {"window", sdbus::Variant{terminal->windowId()}},
                      {"name", sdbus::Variant{info.name}},
                      {"title", sdbus::Variant{info.title}},
                      {"cwd", sdbus::Variant{view != nullptr ? view->cwd() : info.cwd}},
                      {"kind", sdbus::Variant{std::string(kindName(info.kind))}},
                      {"host", sdbus::Variant{info.host}},
                      {"command", sdbus::Variant{command}},
                      {"running", sdbus::Variant{std::holds_alternative<CommandRunning>(info.command)}},
                      {"shown", sdbus::Variant{list.shown() == info.id}},
                      {"split", sdbus::Variant{split != nullptr ? *split : std::vector<std::uint64_t>{}}},
                  });
                }
              }
              return out;
            }),
            sdbus::registerMethod("ListWorkspaces").withOutputParamNames("workspaces").implementedAs([this] {
              std::map<std::uint64_t, std::uint32_t> shownIn;
              for (const auto& [window, workspace] : m_windowWorkspaces) {
                if (const auto* id = std::get_if<std::uint64_t>(&workspace)) shownIn[*id] = m_windows.at(window)->windowId();
              }
              const holder::Workspaces& table = m_workspaces.workspaces();
              std::vector<Entry> out;
              for (const holder::WorkspaceInfo& w : table.workspaces) {
                std::uint32_t sessions = 0, running = 0, failed = 0;
                for (const holder::SessionInfo& s : table.sessions) {
                  if (s.workspace != w.id) continue;
                  ++sessions;
                  if (s.command == holder::CommandState::Running) ++running;
                  if (s.command == holder::CommandState::Done && s.exitCode != 0) ++failed;
                }
                const auto shown = shownIn.find(w.id);
                const char* location = shown != shownIn.end() ? "window" : sessions > 0 ? "detached" : "saved";
                out.push_back(Entry{
                    {"id", sdbus::Variant{w.id}},
                    {"name", sdbus::Variant{w.name}},
                    {"root", sdbus::Variant{w.root}},
                    {"symbol", sdbus::Variant{w.symbol}},
                    {"location", sdbus::Variant{std::string(location)}},
                    {"window", sdbus::Variant{shown != shownIn.end() ? shown->second : 0U}},
                    {"sessions", sdbus::Variant{sessions}},
                    {"running", sdbus::Variant{running}},
                    {"failed", sdbus::Variant{failed}},
                });
              }
              return out;
            }),
            sdbus::registerMethod("Open")
                .withInputParamNames("options")
                .withOutputParamNames("id")
                .implementedAs([this](const Entry& options) { return controlOpen(options); }),
            sdbus::registerMethod("SendText")
                .withInputParamNames("id", "text", "paste")
                .implementedAs([this](std::uint64_t id, const std::string& text, bool paste) {
                  controlTerminal(id).sendText(text, paste);
                }),
            sdbus::registerMethod("GetText")
                .withInputParamNames("id", "extent")
                .withOutputParamNames("text")
                .implementedAs([this](std::uint64_t id, const std::string& extent) {
                  return controlTerminal(id).text(extentOf(extent));
                }),
            sdbus::registerMethod("Focus").withInputParamNames("id").implementedAs([this](std::uint64_t id) {
              controlWindow(id).focusSession(id);
            }),
            sdbus::registerMethod("Close").withInputParamNames("id").implementedAs([this](std::uint64_t id) {
              controlWindow(id).closeSession(id);
            }),
            sdbus::registerMethod("Rename")
                .withInputParamNames("id", "name")
                .implementedAs([this](std::uint64_t id, const std::string& name) {
                  controlWindow(id).renameSession(id, name);
                }),
            sdbus::registerSignal("SessionsChanged"), sdbus::registerSignal("WorkspacesChanged"))
        .forInterface(control::kInterface);
  }

} // namespace term
