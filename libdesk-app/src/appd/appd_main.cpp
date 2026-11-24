// andromeda-appd: the session's app zygote. It holds libdesk, the fonts every app used last and Mesa's EGL driver;
// each launch forks a child that loads the app library (libandromeda-<app>.so) and runs its desk_app_main.

#include "app/app_services.h"
#include "app/desk_app.h"
#include "core/sys/sys.h"
#include "i18n/i18n_service.h"
#include "render/text/cairo_text_renderer.h"

#include <dlfcn.h>
#include <glib.h>
#include <sys/prctl.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

  std::filesystem::path appsDir() {
    const char* dir = std::getenv("ANDROMEDA_APPD_DIR"); // a dev build's directory of app libraries
    return dir != nullptr && dir[0] == '/' ? std::filesystem::path(dir) : std::filesystem::path(ANDROMEDA_APPS_DIR);
  }

  // "andromeda-files" or "/usr/bin/andromeda-files" -> <apps dir>/libandromeda-files.so; empty for other commands.
  std::filesystem::path appLibrary(const std::vector<std::string>& argv) {
    const std::string name = std::filesystem::path(argv.front()).filename().string();
    return name.starts_with("andromeda-") ? appsDir() / ("lib" + name + ".so") : std::filesystem::path();
  }

  std::vector<std::filesystem::path> fontWarmupFiles() {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(DeskApp::fontWarmupDir(), ec)) {
      if (entry.is_regular_file(ec)) {
        files.push_back(entry.path());
      }
    }
    return files;
  }

} // namespace

int main() {
  std::vector<std::string> args = serveAppZygote(
      "appd",
      [] {
        CairoTextRenderer::preloadFonts(fontWarmupFiles());
        i18n::Service::instance().setLanguage({});
      },
      [](const std::vector<std::string>& argv) {
        std::error_code ec;
        const auto library = appLibrary(argv);
        return !library.empty() && std::filesystem::is_regular_file(library, ec);
      }
  );

  const std::string name = std::filesystem::path(args.front()).filename().string();
  prctl(PR_SET_NAME, name.c_str());
  if (gchar* exe = g_find_program_in_path(args.front().c_str())) {
    sys::setExecutablePath(exe);
    g_free(exe);
  }
  const auto library = appLibrary(args);
  void* handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
  using Main = int (*)(int, char**);
  auto* entry = handle != nullptr ? reinterpret_cast<Main>(dlsym(handle, "desk_app_main")) : nullptr;
  if (entry == nullptr) {
    std::fprintf(stderr, "appd: %s: %s\n", library.c_str(), dlerror());
    return 127;
  }
  std::vector<char*> argv;
  for (auto& arg : args) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  return entry(static_cast<int>(args.size()), argv.data());
}
