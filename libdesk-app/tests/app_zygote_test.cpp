// A real zygote serves launches; each child must load fonts the zygote never loaded (Pango's worker runs again in
// the child) and must get the request's working dir and activation token.
#include "core/process/app_zygote.h"
#include "render/text/cairo_text_renderer.h"

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <sstream>
#include <thread>

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
} // namespace

int main() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / std::format("app-zygote-test-{}", getpid());
  std::filesystem::create_directories(dir / "work");
  setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
  std::ofstream(dir / "fonts") << "sans-serif 18px\n";

  const pid_t server = fork();
  if (server == 0) {
    const auto exe = std::filesystem::read_symlink("/proc/self/exe");
    runChild(process::zygote::serve(
        "test.zygote", [&] { CairoTextRenderer::preloadFonts({dir / "fonts"}); },
        [&](const process::zygote::Request& request) { return request.argv.front() == exe.string(); }
    ));
  }

  const auto self = std::filesystem::read_symlink("/proc/self/exe");
  for (int i = 0; i < 3; ++i) {
    const auto result = dir / std::format("result-{}", i);
    const process::zygote::Request request{
        .argv = {self.string(), result.string()}, .workingDir = (dir / "work").string(), .activationToken = std::format("token-{}", i)};
    std::optional<pid_t> child;
    expect(waitFor([&] { return (child = process::zygote::launch("test.zygote", request)).has_value(); }), "zygote never answered");
    expect(waitFor([&] { return !readFile(result).empty(); }), std::format("child {} wrote no result (hung on a font?)", i));
    expect(readFile(result) == std::format("1 {} token-{}", (dir / "work").string(), i),
           std::format("child {} result: '{}'", i, readFile(result)));
  }

  expect(!process::zygote::launch("test.zygote", {.argv = {"env", "A=1", self.string()}}).has_value(),
         "zygote forked a child for another program");

  kill(server, SIGTERM);
  waitpid(server, nullptr, 0);
  std::filesystem::remove_all(dir);
  return g_failures == 0 ? 0 : 1;
}
