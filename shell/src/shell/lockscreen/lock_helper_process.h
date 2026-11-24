#pragma once

#include "config/widget_setting_value.h"
#include "core/process/process.h"

#include <string>
#include <optional>
#include <sys/types.h>
#include <unordered_map>
#include <vector>

// Starts and stops `noctalia lock-helper` (lock_helper.h) for the plugin widgets of one lock.
class LockHelperProcess {
public:
  using Settings = std::unordered_map<std::string, WidgetSettingValue>;

  struct Slot {
    std::string type;
    Settings settings;
    float width = 0.0F;
    float height = 0.0F;
    float scale = 1.0F;
    float contentScale = 1.0F;
  };

  struct StartData {
    bool dark = true;
    std::string timeFormat;
    std::string dateFormat;
    std::vector<std::string> pluginSources;
    std::unordered_map<std::string, Settings> pluginSettings;
  };

  LockHelperProcess() = default;
  ~LockHelperProcess();
  LockHelperProcess(const LockHelperProcess&) = delete;
  LockHelperProcess& operator=(const LockHelperProcess&) = delete;

  // Stops a running helper, then starts a new one. Returns the shell end of each slot's socket,
  // in slot order, owned by the caller; empty when the helper could not start.
  [[nodiscard]] std::vector<int> start(const std::vector<Slot>& slots, const StartData& data);
  // Ends the helper and reaps it. Safe when none runs.
  void stop();

private:
  std::optional<process::Child> m_child;
  int m_alive = -1;
};
