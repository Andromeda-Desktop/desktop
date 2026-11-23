#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

class SessionBus;

namespace sdbus {
  class IProxy;
}

// The compositor's bus name; ANDROMEDA_COMPOSITOR_BUS_NAME names another one (a nested or headless compositor).
[[nodiscard]] std::string compositorBusName();

// Follows the compositor's session phase (Compositor1.Apps `Activity`). The compositor holds each system sleep until
// the session is locked or the shell sends dsk_shell_v1.sleep_ready, so listeners hear a sleep before it starts.
class SessionActivityWatch {
public:
  using SleepListener = std::function<void(bool sleeping)>;

  explicit SessionActivityWatch(SessionBus& bus);
  ~SessionActivityWatch();

  SessionActivityWatch(const SessionActivityWatch&) = delete;
  SessionActivityWatch& operator=(const SessionActivityWatch&) = delete;

  // Runs on each change into "sleeping" or "sleeping-locked" (true) and out of them (false).
  void addSleepListener(SleepListener listener);
  [[nodiscard]] bool sleeping() const noexcept { return m_sleeping; }

private:
  void apply(const std::string& activity);

  std::unique_ptr<sdbus::IProxy> m_apps;
  std::vector<SleepListener> m_listeners;
  bool m_sleeping = false;
  std::shared_ptr<int> m_alive = std::make_shared<int>(0);
};
