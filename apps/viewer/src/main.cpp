#include "viewer_app.h"

int main(int argc, char* argv[]) {
  if (const auto exit = DeskApp::forwardToRunning(viewer::ViewerApp::kAppId, argc, argv)) {
    return *exit;
  }
  viewer::ViewerApp app;
  return app.run(argc, argv);
}
