#include "terminal_control.h"
#include "terminal_app.h"

#include <nlohmann/json.hpp>

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
      throw control::Error(name, message);
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

  std::uint64_t TerminalApp::controlOpen(const nlohmann::json& options) {
    NewShell shell;
    std::string name;
    std::optional<std::uint32_t> windowId;
    for (const auto& [key, value] : options.items()) {
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

  nlohmann::json TerminalApp::controlCall(const std::string& method, const nlohmann::json& args) {
    using Json = nlohmann::json;
    try {
      if (method == "ListSessions") {
        Json out = Json::array();
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
            out.push_back(Json{
                {"id", info.id},
                {"window", terminal->windowId()},
                {"name", info.name},
                {"title", info.title},
                {"cwd", view != nullptr ? view->cwd() : info.cwd},
                {"kind", std::string(kindName(info.kind))},
                {"host", info.host},
                {"command", command},
                {"running", std::holds_alternative<CommandRunning>(info.command)},
                {"shown", list.shown() == info.id},
                {"split", split != nullptr ? *split : std::vector<std::uint64_t>{}},
            });
          }
        }
        return out;
      }
      if (method == "ListWorkspaces") {
        std::map<std::uint64_t, std::uint32_t> shownIn;
        for (const auto& [window, terminal] : m_windows) {
          if (const auto id = m_windowWorkspaces.workspaceOf(window)) shownIn[*id] = terminal->windowId();
        }
        const holder::Workspaces& table = m_workspaces.workspaces();
        Json out = Json::array();
        for (const holder::WorkspaceInfo& w : table.workspaces) {
          std::uint32_t sessions = 0, running = 0, failed = 0;
          for (const holder::SessionInfo& s : table.sessions) {
            if (s.workspace != w.id) continue;
            ++sessions;
            if (s.command == holder::CommandState::Running) ++running;
            if (s.command == holder::CommandState::Done && s.exitCode != 0) ++failed;
          }
          const auto shown = shownIn.find(w.id);
          const char* location = shown != shownIn.end() ? "window" : "none"; // none: start-up or a closing window
          out.push_back(Json{
              {"id", w.id},
              {"name", w.name},
              {"root", w.root},
              {"symbol", w.symbol},
              {"location", std::string(location)},
              {"window", shown != shownIn.end() ? shown->second : 0U},
              {"sessions", sessions},
              {"running", running},
              {"failed", failed},
          });
        }
        return out;
      }
      if (method == "Open") {
        return controlOpen(args.at(0));
      }
      const auto id = args.at(0).get<std::uint64_t>();
      if (method == "SendText") {
        controlTerminal(id).sendText(args.at(1).get<std::string>(), args.at(2).get<bool>());
      } else if (method == "GetText") {
        return controlTerminal(id).text(extentOf(args.at(1).get<std::string>()));
      } else if (method == "Focus") {
        controlWindow(id).focusSession(id);
      } else if (method == "Close") {
        controlWindow(id).closeSession(id);
      } else if (method == "Rename") {
        controlWindow(id).renameSession(id, args.at(1).get<std::string>());
      } else {
        fail(control::kUnknownMethod, std::format("no method '{}'", method));
      }
      return nullptr;
    } catch (const nlohmann::json::exception& e) {
      fail(control::kInvalidArgs, e.what());
    }
  }

} // namespace term
