#pragma once

#include "app/color_scheme.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

class AppLoop;
class SessionBus;

// The desktop session services of one app process: single instance, the appearance settings, accessibility.
// Linux: session bus (D-Bus activation, the settings portal, AT-SPI). macOS: none yet.
class AppServices {
public:
  struct Callbacks {
    std::function<void(const std::string& token)> activate;
    std::function<void(const std::vector<std::string>& uris, const std::string& token)> open;
    std::function<void(const std::string& name, const std::vector<std::string>& parameter, const std::string& token)>
        action;
    std::function<void()> appearanceChanged;
    std::function<void()> sessionLost; // the session ended under the app: quit and flush
  };

  virtual ~AppServices() = default;

  // Claims the app id for this process; false when a running instance took this launch.
  [[nodiscard]] virtual bool claim(const std::vector<std::string>& uris, const std::string& action) = 0;
  // Adds the services' poll sources to the loop; detach() removes them.
  virtual void attach(AppLoop& loop) = 0;
  virtual void detach(AppLoop& loop) = 0;
  // Applies replies that already arrived (the appearance), so the first window is built once.
  virtual void processPendingEvents() = 0;
  [[nodiscard]] virtual bool hasAppearance() const = 0;
  [[nodiscard]] virtual float uiScale() const = 0;
  [[nodiscard]] virtual float textScale() const = 0;
  [[nodiscard]] virtual bool holdMotion() const = 0;
  virtual void setColorScheme(ColorScheme scheme) = 0;
  // Null when the platform has no session bus or it is not reachable.
  [[nodiscard]] virtual SessionBus* bus() = 0;
  // Shows the files (absolute paths), selected, in the desktop's file manager.
  virtual void showInFolder(const std::vector<std::string>& paths) = 0;
};

// singleInstance false: claim() always takes the launch.
[[nodiscard]] std::unique_ptr<AppServices> createAppServices(
    const std::string& appId, const std::string& appName, bool singleInstance, AppServices::Callbacks callbacks
);
// Before the app object exists: true when a running instance took this launch.
[[nodiscard]] bool forwardLaunch(const std::string& appId, const std::vector<std::string>& uris, const std::string& action);
// andromeda-appd: forks one child per launch request that accept() takes, after warmup; returns the child's argv.
// Empty where there is no zygote.
[[nodiscard]] std::vector<std::string> serveAppZygote(
    const std::string& name, const std::function<void()>& warmup,
    const std::function<bool(const std::vector<std::string>& argv)>& accept
);
