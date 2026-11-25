#include "cli/ipc_client.h"

#include "protocols/ipc_channel.h"
#include "protocols/xdg_dirs.h"
#include "server/ipc_commands.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <print>
#include <string>
#include <utility>
#include <vector>

namespace andromeda {

  namespace {
    std::string resolveSocketPath() {
      if (const char* sock = std::getenv("ANDROMEDA_SOCKET"); sock != nullptr && sock[0] != '\0') {
        return sock;
      }
      const std::string runtimeDir = xdg::runtimeDir();
      const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
      if (!runtimeDir.empty() && waylandDisplay != nullptr && waylandDisplay[0] != '\0') {
        return runtimeDir + "/andromeda-" + waylandDisplay + ".sock";
      }
      return {};
    }

    // Replies and event lines from the compositor; a full window list stays far below this.
    constexpr size_t kMaxReplyBytes = 64 * 1024 * 1024;
    constexpr auto kConnectTimeout = std::chrono::seconds(2);

    std::string requestPayload(nlohmann::json request) {
      if (const char* token = std::getenv("ANDROMEDA_LAUNCH_TOKEN"); token != nullptr && token[0] != '\0') {
        request["launch_token"] = token;
      }
      return request.dump() + "\n";
    }
  } // namespace

  int runIpcCommand(const IpcCommandSpec& spec, std::string_view arg, bool json) {
    const std::string socketPath = resolveSocketPath();
    if (socketPath.empty()) {
      std::println(stderr, "error: cannot find andromeda socket (is the compositor running?)");
      return EXIT_FAILURE;
    }
    nlohmann::json req;
    req["cmd"] = spec.name;
    if (spec.takesArg) {
      req["arg"] = std::string(arg);
    }
    // A one-shot request answers within its command's limit or not at all; the event stream deliberately skips this.
    const auto reply = ipc::request(
        socketPath, requestPayload(std::move(req)), ipc::EndOfRequest::KeepOpen, kMaxReplyBytes,
        std::chrono::steady_clock::now() + std::chrono::seconds(spec.replyTimeoutSec)
    );
    switch (reply.status) {
      case ipc::Status::Ok:
        break;
      case ipc::Status::NoServer:
        std::println(stderr, "error: cannot connect to andromeda socket (is the compositor running?)");
        return EXIT_FAILURE;
      case ipc::Status::TimedOut:
        std::println(stderr, "error: no reply within {} s", spec.replyTimeoutSec);
        return EXIT_FAILURE;
      case ipc::Status::TooLong:
      case ipc::Status::Failed:
        std::println(stderr, "error: malformed response");
        return EXIT_FAILURE;
    }
    std::string buf = reply.data;

    if (buf.empty()) {
      std::println(stderr, "error: malformed response");
      return EXIT_FAILURE;
    }

    // Strip trailing newline.
    if (buf.back() == '\n') {
      buf.pop_back();
    }

    auto resp = nlohmann::json::parse(buf, nullptr, false);
    if (resp.is_discarded()) {
      std::println(stderr, "error: malformed response");
      return EXIT_FAILURE;
    }

    if (resp.contains("err")) {
      std::println(stderr, "error: {}", resp["err"].get<std::string>());
      return EXIT_FAILURE;
    }

    if (!resp.contains("ok")) {
      std::println(stderr, "error: malformed response");
      return EXIT_FAILURE;
    }

    const auto& ok = resp["ok"];

    if (json) {
      std::println("{}", ok.dump());
      return EXIT_SUCCESS;
    }

    if (spec.printHuman != nullptr) {
      spec.printHuman(ok);
    } else if (spec.group == IpcCommandGroup::Inspect) {
      std::println("{}", ok.dump(2));
    }

    return EXIT_SUCCESS;
  }

  int runIpcSubscribe(const std::vector<std::string>& events) {
    const std::string socketPath = resolveSocketPath();
    if (socketPath.empty()) {
      std::println(stderr, "error: cannot find andromeda socket (is the compositor running?)");
      return EXIT_FAILURE;
    }
    const auto deadline = std::chrono::steady_clock::now() + kConnectTimeout;
    const ipc::Fd fd = ipc::connectTo(socketPath, deadline);
    if (!fd) {
      std::println(stderr, "error: cannot connect to andromeda socket (is the compositor running?)");
      return EXIT_FAILURE;
    }

    nlohmann::json req;
    req["cmd"] = "subscribe";
    req["events"] = events;
    if (!ipc::writeAll(fd.get(), requestPayload(std::move(req)), deadline)) {
      std::println(stderr, "error: failed to send request");
      return EXIT_FAILURE;
    }

    // Deliberately no read deadline: a stream is idle exactly as often as the compositor is. Lines are relayed
    // unparsed, except the first, which is the rejection when a subscription name is unknown - the compositor
    // answers that and closes, so no event can be mistaken for it.
    ipc::LineReader reader(kMaxReplyBytes);
    bool sawLine = false;
    while (reader.readFrom(fd.get()) > 0) {
      while (auto line = reader.nextLine()) {
        if (!sawLine) {
          sawLine = true;
          const auto parsed = nlohmann::json::parse(*line, nullptr, false);
          if (!parsed.is_discarded() && parsed.is_object() && parsed.contains("err")) {
            std::println(stderr, "error: {}", parsed["err"].get<std::string>());
            return EXIT_FAILURE;
          }
        }
        std::println("{}", *line);
        // Line-buffered by hand: a bar reading this pipe must see an event when it happens, and a closed reader
        // ends the stream instead of filling a buffer nobody drains.
        if (std::fflush(stdout) != 0) {
          return EXIT_FAILURE;
        }
      }
    }

    if (!sawLine) {
      std::println(stderr, "error: compositor closed the stream without sending anything");
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  }

} // namespace andromeda
