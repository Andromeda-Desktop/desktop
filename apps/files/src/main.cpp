#include "files_app.h"
#include "network_page.h"
#include "vaults.h"

#include <glib.h>

#include <string>
#include <string_view>
#include <vector>

int main(int argc, char* argv[]) {
  std::vector<std::string> childArgs;
  std::vector<char*> childArgv;
  if (argc == 2 && std::string_view(argv[1]) == "--zygote") {
    childArgs = DeskApp::serveZygote(FilesApp::kAppId);
    for (auto& arg : childArgs) {
      childArgv.push_back(arg.data());
    }
    childArgv.push_back(nullptr);
    argc = static_cast<int>(childArgs.size());
    argv = childArgv.data();
  }
  bool picker = false;
  bool service = false; // D-Bus activation for org.freedesktop.FileManager1: no window until a request
  for (int i = 1; i < argc; ++i) {
    picker = picker || std::string_view(argv[i]) == "--picker";
    service = service || std::string_view(argv[i]) == "--service";
  }
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--vault-guard" && i + 1 < argc) { // locks an unlocked vault before sleep
      return Vaults::guard(argv[i + 1]);
    }
    if (std::string_view(argv[i]) == "--reconnect") { // login: mount the saved servers marked "connect at login"
      GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
      int failed = 0;
      bool finished = false; // no saved server answers before the loop runs
      NetworkPage::reconnectSaved([&](int, int f) {
        failed = f;
        finished = true;
        g_main_loop_quit(loop);
      });
      if (!finished) {
        g_main_loop_run(loop);
      }
      g_main_loop_unref(loop);
      return failed == 0 ? 0 : 1;
    }
  }
  if (!service) {
    if (const auto exit = DeskApp::forwardToRunning(picker ? FilesApp::kPickerAppId : FilesApp::kAppId, argc, argv)) {
      return *exit;
    }
  }
  FilesApp app(picker, service);
  return app.run(argc, argv);
}
