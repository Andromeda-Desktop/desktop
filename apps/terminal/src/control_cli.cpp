#include "terminal_control.h"

#include <cstdio>
#include <iostream>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <sdbus-c++/sdbus-c++.h>
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

    nlohmann::json toJson(const sdbus::Variant& value) {
      const std::string type = value.peekValueType();
      if (type == "t") {
        return value.get<std::uint64_t>();
      }
      if (type == "u") {
        return value.get<std::uint32_t>();
      }
      if (type == "b") {
        return value.get<bool>();
      }
      if (type == "at") {
        return value.get<std::vector<std::uint64_t>>();
      }
      return value.get<std::string>();
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
      auto bus = sdbus::createSessionBusConnection();
      auto proxy = sdbus::createProxy(*bus, sdbus::ServiceName{kService}, sdbus::ObjectPath{kPath});
      // The trailing ID of a command; nullopt when it is missing or no number.
      const auto idAt = [&](std::size_t i) { return i < args.size() ? parseId(args[i]) : std::nullopt; };

      if ((verb == "ls" || verb == "workspaces") && args.empty()) {
        std::vector<std::map<std::string, sdbus::Variant>> entries;
        proxy->callMethod(verb == "ls" ? "ListSessions" : "ListWorkspaces").onInterface(kInterface).storeResultsTo(entries);
        nlohmann::json out = nlohmann::json::array();
        for (const auto& entry : entries) {
          nlohmann::json item = nlohmann::json::object();
          for (const auto& [key, value] : entry) {
            item[key] = toJson(value);
          }
          out.push_back(std::move(item));
        }
        std::cout << out.dump(2) << '\n';
        return 0;
      }
      if (verb == "open") {
        std::map<std::string, sdbus::Variant> options;
        for (std::size_t i = 0; i < args.size(); ++i) {
          const std::string& arg = args[i];
          const bool hasValue = i + 1 < args.size();
          if (arg == "--") {
            options["argv"] = sdbus::Variant{std::vector<std::string>(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end())};
            break;
          }
          if (arg == "--new-window") {
            options["window"] = sdbus::Variant{std::uint32_t{0}};
          } else if ((arg == "--cwd" || arg == "--name") && hasValue) {
            options[arg.substr(2)] = sdbus::Variant{args[++i]};
          } else if (const auto window = arg == "--window" && hasValue ? parseId(args[++i]) : std::nullopt;
                     window && *window > 0 && *window <= UINT32_MAX) {
            options["window"] = sdbus::Variant{static_cast<std::uint32_t>(*window)};
          } else {
            return usage();
          }
        }
        std::uint64_t id = 0;
        proxy->callMethod("Open").onInterface(kInterface).withArguments(options).storeResultsTo(id);
        std::cout << id << '\n';
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
        proxy->callMethod("SendText").onInterface(kInterface).withArguments(*id, text, paste);
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
        std::string text;
        proxy->callMethod("GetText").onInterface(kInterface).withArguments(*id, extent).storeResultsTo(text);
        std::cout << text << (text.ends_with('\n') ? "" : "\n");
        return 0;
      }
      if ((verb == "focus" || verb == "close") && args.size() == 1 && idAt(0)) {
        proxy->callMethod(verb == "focus" ? "Focus" : "Close").onInterface(kInterface).withArguments(*idAt(0));
        return 0;
      }
      if (verb == "rename" && args.size() == 2 && idAt(0)) {
        proxy->callMethod("Rename").onInterface(kInterface).withArguments(*idAt(0), args[1]);
        return 0;
      }
      return usage();
    } catch (const sdbus::Error& e) {
      if (e.getName() == "org.freedesktop.DBus.Error.ServiceUnknown") {
        std::fputs("andromeda-terminal ctl: no terminal is running\n", stderr);
      } else {
        std::fprintf(stderr, "andromeda-terminal ctl: %s\n", e.getMessage().c_str());
      }
      return 1;
    }
  }

} // namespace term::control
