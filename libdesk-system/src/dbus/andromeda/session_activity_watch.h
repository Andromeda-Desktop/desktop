#pragma once

#include <functional>
#include <optional>
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
  using ChangeListener = std::function<void()>;

  explicit SessionActivityWatch(SessionBus& bus);
  ~SessionActivityWatch();

  SessionActivityWatch(const SessionActivityWatch&) = delete;
  SessionActivityWatch& operator=(const SessionActivityWatch&) = delete;

  // Runs on each change into "sleeping" or "sleeping-locked" (true) and out of them (false).
  void addSleepListener(SleepListener listener);
  [[nodiscard]] bool sleeping() const noexcept { return m_sleeping; }
  // Compositor1.Apps Presenting: an app captures the screen, or an output mirrors another (a projector). nullopt
  // until the compositor answers, and on a compositor without the interface.
  [[nodiscard]] std::optional<bool> presenting() const noexcept { return m_presenting; }
  // Runs after Presenting changes.
  void addChangeListener(ChangeListener listener) { m_changeListeners.push_back(std::move(listener)); }

private:
  void apply(const std::string& activity);
  void applyPresenting(bool presenting);

  std::unique_ptr<sdbus::IProxy> m_apps;
  std::vector<SleepListener> m_listeners;
  std::vector<ChangeListener> m_changeListeners;
  bool m_sleeping = false;
  std::optional<bool> m_presenting;
  std::shared_ptr<int> m_alive = std::make_shared<int>(0);
};
