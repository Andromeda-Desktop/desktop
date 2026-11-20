#include "terminal_control.h"

#include <cstdio>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace term::control {

  namespace {

    constexpr std::string_view kUsage = R"(usage: andromeda-terminal ctl COMMAND
  ls                                   the sessions of the running terminal, as JSON
  workspaces                           the workspaces of the session holder, as JSON
  open [--cwd DIR] [--window N | --new-window] [--name NAME] [-- COMMAND...]
                                       a new session; prints its id
  send-text [--paste] ID TEXT          TEXT as typed ("-": stdin); --paste sends it as a paste
  get-text [--extent EXTENT] ID        screen (default), all, selection or last_output
  focus ID | close ID | rename ID NAME
)";

    int usage() {
      std::fputs(kUsage.data(), stderr);
      return 2;
    }

    std::optional<std::uint64_t> parseId(const std::string& text) {
      try {
        std::size_t used = 0;
        const unsigned long long id = std::stoull(text, &used);
        return used == text.size() ? std::optional<std::uint64_t>(id) : std::nullopt;
      } catch (const std::exception&) {
        return std::nullopt;
      }
    }

  } // namespace

  std::optional<int> runCli(int argc, char* argv[]) {
    if (argc < 2 || std::string_view(argv[1]) != "ctl") {
      return std::nullopt;
    }
    std::vector<std::string> args(argv + 2, argv + argc);
    if (args.empty()) {
      return usage();
    }
    const std::string verb = args.front();
    args.erase(args.begin());
    try {
      // The trailing ID of a command; nullopt when it is missing or no number.
      const auto idAt = [&](std::size_t i) { return i < args.size() ? parseId(args[i]) : std::nullopt; };

      if ((verb == "ls" || verb == "workspaces") && args.empty()) {
        std::cout << call(verb == "ls" ? "ListSessions" : "ListWorkspaces", nlohmann::json::array()).dump(2) << '\n';
        return 0;
      }
      if (verb == "open") {
        nlohmann::json options = nlohmann::json::object();
        for (std::size_t i = 0; i < args.size(); ++i) {
          const std::string& arg = args[i];
          const bool hasValue = i + 1 < args.size();
          if (arg == "--") {
            options["argv"] = std::vector<std::string>(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end());
            break;
          }
          if (arg == "--new-window") {
            options["window"] = 0;
          } else if ((arg == "--cwd" || arg == "--name") && hasValue) {
            options[arg.substr(2)] = args[++i];
          } else if (const auto window = arg == "--window" && hasValue ? parseId(args[++i]) : std::nullopt;
                     window && *window > 0 && *window <= UINT32_MAX) {
            options["window"] = static_cast<std::uint32_t>(*window);
          } else {
            return usage();
          }
        }
        std::cout << call("Open", nlohmann::json::array({options})).get<std::uint64_t>() << '\n';
        return 0;
      }
      if (verb == "send-text") {
        const bool paste = !args.empty() && args.front() == "--paste";
        const std::size_t at = paste ? 1 : 0;
        const auto id = idAt(at);
        if (!id || args.size() != at + 2) {
          return usage();
        }
        std::string text = args[at + 1];
        if (text == "-") {
          text.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
        }
        call("SendText", {*id, text, paste});
        return 0;
      }
      if (verb == "get-text") {
        std::string extent = "screen";
        std::size_t at = 0;
        if (args.size() == 3 && args.front() == "--extent") {
          extent = args[1];
          at = 2;
        }
        const auto id = idAt(at);
        if (!id || args.size() != at + 1) {
          return usage();
        }
        const std::string text = call("GetText", {*id, extent}).get<std::string>();
        std::cout << text << (text.ends_with('\n') ? "" : "\n");
        return 0;
      }
      if ((verb == "focus" || verb == "close") && args.size() == 1 && idAt(0)) {
        call(verb == "focus" ? "Focus" : "Close", {*idAt(0)});
        return 0;
      }
      if (verb == "rename" && args.size() == 2 && idAt(0)) {
        call("Rename", {*idAt(0), args[1]});
        return 0;
      }
      return usage();
    } catch (const Error& e) {
      if (e.name == kNotRunning) {
        std::fputs("andromeda-terminal ctl: no terminal is running\n", stderr);
      } else {
        std::fprintf(stderr, "andromeda-terminal ctl: %s\n", e.what());
      }
      return 1;
    }
  }

} // namespace term::control
