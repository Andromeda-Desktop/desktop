// A real zygote serves launches; each child must load fonts the zygote never loaded (Pango's worker runs again in
// the child) and must get the request's working dir and activation token.
#include "core/process/app_zygote.h"
#include "render/text/cairo_text_renderer.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <print>
#include <sstream>
#include <thread>
#include <variant>

namespace {
  int g_failures = 0;

  void expect(bool ok, const std::string& what) {
    if (!ok) {
      std::println(stderr, "app_zygote_test: {}", what);
      ++g_failures;
    }
  }

  template <typename F> bool waitFor(F done) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done()) {
      if (std::chrono::steady_clock::now() > until) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
  }

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
  }

  [[noreturn]] void runChild(const process::zygote::Request& request) {
    alarm(5); // a missing Pango worker hangs the first unseen font
    CairoTextRenderer text;
    text.initialize(nullptr, nullptr);
    const auto unseenSize = text.measureFont(1.0F, 31.0F);
    const auto unseenFamily = text.measureFont(1.0F, 22.0F, TextFont{.weight = FontWeight::Bold, .family = "serif"});
    const char* token = std::getenv("XDG_ACTIVATION_TOKEN");
    std::ofstream(request.argv.at(1)) << (unseenSize.bottom > unseenSize.top && unseenFamily.bottom > unseenFamily.top)
                                      << ' ' << std::filesystem::current_path().string() << ' '
                                      << (token != nullptr ? token : "-");
    std::_Exit(0);
  }

  // A zygote that replies to the launch only after the client's 1 s wait, then answers the status query with
  // statusReply (or never, when absent). Returns its pid.
  pid_t runSlowZygote(std::string_view appId, std::optional<std::int32_t> statusReply) {
    const std::string path = process::zygote::socketPath(appId).string();
    const int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    sockaddr_un addr{.sun_family = AF_UNIX, .sun_path = {}};
    std::memcpy(addr.sun_path, path.c_str(), path.size());
    expect(bind(listener, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0 && listen(listener, 4) == 0,
           "slow zygote: listen");
    const pid_t pid = fork();
    if (pid != 0) {
      close(listener);
      return pid;
    }
    std::array<char, 4096> buffer{};
    const int launchConn = accept(listener, nullptr, nullptr);
    const auto launch = process::zygote::decode(std::span(buffer.data(), std::max<ssize_t>(0, recv(launchConn, buffer.data(), buffer.size(), 0))));
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    const std::int32_t started = 4242;
    send(launchConn, &started, sizeof(started), MSG_NOSIGNAL);
    const int statusConn = accept(listener, nullptr, nullptr);
    const auto status = process::zygote::decode(std::span(buffer.data(), std::max<ssize_t>(0, recv(statusConn, buffer.data(), buffer.size(), 0))));
    const bool sameId = launch && status && std::get<process::zygote::Request>(*launch).id
                                                == std::get<process::zygote::StatusQuery>(*status).id;
    if (statusReply && sameId) {
      send(statusConn, &*statusReply, sizeof(*statusReply), MSG_NOSIGNAL);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    }
    std::_Exit(0);
  }
} // namespace

int main() {
  using enum process::zygote::LaunchResult::Outcome;
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / std::format("app-zygote-test-{}", getpid());
  std::filesystem::create_directories(dir / "work");
  setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
  std::ofstream(dir / "fonts") << "sans-serif 18px\n";

  const pid_t server = fork();
  if (server == 0) {
    const auto exe = std::filesystem::read_symlink("/proc/self/exe");
    runChild(process::zygote::serve(
        "test.zygote", [&] { CairoTextRenderer::preloadFonts({dir / "fonts"}); },
        [&](const process::zygote::Request& request) {
          if (request.activationToken == "slow") {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500)); // a launch queued behind a slow one
          }
          return request.argv.front() == exe.string();
        }
    ));
  }

  const auto self = std::filesystem::read_symlink("/proc/self/exe");
  // A same-user client that connects and never sends must not stop the launches below.
  const int silent = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  sockaddr_un addr{.sun_family = AF_UNIX, .sun_path = {}};
  const std::string socket = process::zygote::socketPath("test.zygote").string();
  std::memcpy(addr.sun_path, socket.c_str(), socket.size());
  expect(waitFor([&] { return connect(silent, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0; }),
         "zygote socket never appeared");
  for (int i = 0; i < 3; ++i) {
    const auto result = dir / std::format("result-{}", i);
    const process::zygote::Request request{
        .argv = {self.string(), result.string()}, .workingDir = (dir / "work").string(), .activationToken = std::format("token-{}", i)};
    expect(waitFor([&] { return process::zygote::launch("test.zygote", request).outcome == Started; }), "zygote never answered");
    expect(waitFor([&] { return !readFile(result).empty(); }), std::format("child {} wrote no result (hung on a font?)", i));
    expect(readFile(result) == std::format("1 {} token-{}", (dir / "work").string(), i),
           std::format("child {} result: '{}'", i, readFile(result)));
  }

  expect(process::zygote::launch("test.zygote", {.argv = {"env", "A=1", self.string()}}).outcome == Refused,
         "zygote forked a child for another program");

  // A launch the client gave up on must not start a child; its status query says refused, so the client may start
  // the app itself.
  const auto late = dir / "result-late";
  expect(process::zygote::launch("test.zygote", {.argv = {self.string(), late.string()}, .activationToken = "slow"}).outcome
             == Refused,
         "a launch slower than the client's wait was not reported as refused");
  const auto after = dir / "result-after";
  expect(waitFor([&] { return process::zygote::launch("test.zygote", {.argv = {self.string(), after.string()}}).outcome == Started; }),
         "zygote never answered after a late launch");
  expect(waitFor([&] { return !readFile(after).empty(); }), "the launch after a late one wrote no result");
  expect(!std::filesystem::exists(late), "the zygote started a child for a launch the client gave up on");

  // A child started but its reply came after the client's wait: the status query reports it, so the caller never
  // starts a second copy. With no status answer the outcome stays unknown, which also forbids a second start.
  const pid_t delayed = runSlowZygote("test.delayed", 4242);
  const auto delayedLaunch = process::zygote::launch("test.delayed", {.argv = {self.string()}});
  expect(delayedLaunch.outcome == Started && delayedLaunch.pid == 4242, "a started launch with a late reply was not reported as started");
  waitpid(delayed, nullptr, 0);
  const pid_t mute = runSlowZygote("test.mute", std::nullopt);
  expect(process::zygote::launch("test.mute", {.argv = {self.string()}}).outcome == Unknown,
         "a launch with no reply and no status was not reported as unknown");
  waitpid(mute, nullptr, 0);

  close(silent);
  kill(server, SIGTERM);
  waitpid(server, nullptr, 0);
  std::filesystem::remove_all(dir);
  return g_failures == 0 ? 0 : 1;
}
