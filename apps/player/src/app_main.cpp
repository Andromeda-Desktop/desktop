#include "app/app_main.h"
#include "player_app.h"

int desk_app_main(int argc, char* argv[]) {
  if (const auto exit = DeskApp::forwardToRunning(PlayerApp::kAppId, argc, argv)) {
    return *exit;
  }
  PlayerApp app;
  return app.run(argc, argv);
}
