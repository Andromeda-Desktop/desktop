#include "player_app.h"

int main(int argc, char* argv[]) {
  if (const auto exit = DeskApp::forwardToRunning(PlayerApp::kAppId, argc, argv)) {
    return *exit;
  }
  PlayerApp app;
  return app.run(argc, argv);
}
