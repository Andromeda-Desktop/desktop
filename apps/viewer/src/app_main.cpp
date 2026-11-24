#include "app/app_main.h"
#include "viewer_app.h"

int desk_app_main(int argc, char* argv[]) {
  if (const auto exit = DeskApp::forwardToRunning(viewer::ViewerApp::kAppId, argc, argv)) {
    return *exit;
  }
  viewer::ViewerApp app;
  return app.run(argc, argv);
}
