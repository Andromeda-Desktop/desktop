#include "app/app_main.h"
#include "terminal_app.h"
#include "terminal_control.h"
#include "theme_cli.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

  // Terminal options in the form other terminals and xdg-terminal-exec use, as DeskApp arguments:
  // -e CMD ARGS... runs CMD (the "exec" action, so a running instance gets it too); --working-directory DIR
  // starts there.
  std::vector<std::string> deskArgs(int argc, char* argv[]) {
    std::vector<std::string> out{argv[0]};
    std::string dir;
    for (int i = 1; i < argc; ++i) {
      const std::string_view arg = argv[i];
      if (arg.starts_with("--working-directory=")) {
        dir = arg.substr(std::string_view("--working-directory=").size());
      } else if (arg == "--working-directory" && i + 1 < argc) {
        dir = argv[++i];
      } else if (arg == "-e" && i + 1 < argc) {
        std::error_code ec;
        const std::filesystem::path cwd =
            std::filesystem::absolute(dir.empty() ? std::filesystem::current_path(ec) : std::filesystem::path(dir), ec);
        out.insert(out.end(), {std::string("--action=exec"), std::string("--"), cwd.string()});
        out.insert(out.end(), argv + i + 1, argv + argc);
        return out;
      } else {
        out.emplace_back(arg);
      }
    }
    if (!dir.empty()) {
      out.push_back(dir);
    }
    return out;
  }

} // namespace

int desk_app_main(int argc, char* argv[]) {
  if (const auto exit = term::runThemeCli(argc, argv)) {
    return *exit;
  }
  if (const auto exit = term::control::runCli(argc, argv)) {
    return *exit;
  }
  std::vector<std::string> args = deskArgs(argc, argv);
  std::vector<char*> raw;
  for (std::string& arg : args) {
    raw.push_back(arg.data());
  }
  raw.push_back(nullptr);
  if (const auto exit =
          DeskApp::forwardToRunning(term::TerminalApp::kAppId, static_cast<int>(args.size()), raw.data())) {
    return *exit;
  }
  term::TerminalApp app;
  return app.run(static_cast<int>(args.size()), raw.data());
}
