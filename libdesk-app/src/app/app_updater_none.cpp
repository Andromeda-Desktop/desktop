#include "app/app_updater.h"

std::unique_ptr<AppUpdater> makeAppUpdater(AppLoop& /*loop*/, const std::string& /*manifestUrl*/,
                                           std::function<void(const std::string&)> /*onReady*/) {
  return nullptr;
}
