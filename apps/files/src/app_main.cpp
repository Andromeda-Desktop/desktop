#include "app/app_main.h"
#include "app/glib_source.h"
#include "core/poll_loop.h"
#include "files_app.h"
#include "network_page.h"
#include "vaults.h"

#include <string_view>

int desk_app_main(int argc, char* argv[]) {
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
      PollLoop loop;
      GlibSource glib;
      loop.addSource(&glib);
      int failed = 0;
      // With no saved server, done runs before run(): the quit makes run() return at once.
      NetworkPage::reconnectSaved([&](int, int f) {
        failed = f;
        loop.quit();
      });
      loop.run();
      loop.removeSource(&glib);
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
