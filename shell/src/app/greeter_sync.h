#pragma once

#include "config/config_types.h"
#include "core/timer_manager.h"

#include <cstdint>
#include <functional>
#include <string>

class CompositorPlatform;
class ConfigService;
namespace greeter {
  enum class GreeterSyncLaunch;
}
namespace noctalia::theme {
  class ThemeService;
}

// Copies the shell look to the greeter: a manual sync from Settings, and a quiet auto sync 1 s after a look change.
// A newer sync makes the callbacks of an older one stale (generation).
class GreeterSync {
public:
  struct Hooks {
    std::function<bool()> logindOnSystemBus;
    std::function<bool()> polkitAgentActive;
    std::function<bool()> inSessionPolkit;
    std::function<void(const std::string&)> settingsError;
  };

  GreeterSync(
      ConfigService& config, noctalia::theme::ThemeService& theme, CompositorPlatform& platform, Hooks hooks
  );
  ~GreeterSync();
  GreeterSync(const GreeterSync&) = delete;
  GreeterSync& operator=(const GreeterSync&) = delete;

  // Takes the current lock screen look as the one the greeter has, and auto syncs when a lock wallpaper bake ends.
  void start();
  void sync(bool quiet = false);
  void scheduleAuto();
  // Auto syncs when the lock screen config changed since the last sync.
  void onConfigReload();

private:
  void reportFailure(bool quiet);
  // Main loop, after the staging job of the current sync.
  void onLaunched(std::uint64_t generation, bool quiet, greeter::GreeterSyncLaunch launch);

  ConfigService& m_config;
  noctalia::theme::ThemeService& m_theme;
  CompositorPlatform& m_platform;
  Hooks m_hooks;
  Timer m_timeoutTimer;
  Timer m_autoTimer;
  LockscreenConfig m_lockscreen;
  bool m_clock24h = true; // the greeter clock follows the system hour cycle
  std::uint64_t m_generation = 0;
};
