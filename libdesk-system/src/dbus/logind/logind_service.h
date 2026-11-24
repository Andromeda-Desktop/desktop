#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "protocols/ipc_channel.h"

class SystemBus;

namespace sdbus {
  class IProxy;
  class ObjectPath;
}

class LogindService {
public:
  using SessionLockCallback = std::function<void()>;
  using IdleInhibitCallback = std::function<void()>;
  // Gets the lock fd (CLOEXEC; closing it releases the lock), or an empty Fd when logind refused.
  using InhibitCallback = std::function<void(ipc::Fd)>;

  static constexpr std::string_view kBusName = "org.freedesktop.login1";

  explicit LogindService(SystemBus& bus);
  ~LogindService();

  void setLockCallback(SessionLockCallback callback);
  void setUnlockCallback(SessionLockCallback callback);

  void setSessionLockIntegrationEnabled(bool enabled);
  void setSessionLockedHint(bool locked);

  // Idle inhibit is async: logind can hang (ACPI), so the main thread never waits for it.
  // The callback runs on the main thread when a request reaches Held; a failure keeps the Wayland fallback.
  enum class IdleInhibit : std::uint8_t { None, Pending, PendingCancelled, Held };
  [[nodiscard]] bool supportsIdleInhibit() const noexcept;
  [[nodiscard]] bool hasIdleInhibit() const noexcept { return m_idleInhibit == IdleInhibit::Held; }
  void setIdleInhibitCallback(IdleInhibitCallback callback);
  void acquireIdleInhibit();
  void releaseIdleInhibit();

  // The one owner of logind Inhibit (mode "block" or "delay"). Direct, not through System1: logind applies a lock
  // to its caller's session. Async: logind can hang. The callback never runs after this service is gone; a caller
  // that can die first checks its own lifetime.
  void inhibit(std::string what, std::string who, std::string why, std::string mode, InhibitCallback callback);

private:
  void ensureSessionLockMonitor();
  void attachSessionProxy(const sdbus::ObjectPath& sessionPath);

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_managerProxy;
  std::unique_ptr<sdbus::IProxy> m_sessionProxy;
  bool m_sessionLockIntegrationEnabled = false;
  bool m_sessionResolving = false;
  std::optional<bool> m_pendingLockedHint;
  std::shared_ptr<int> m_aliveToken = std::make_shared<int>(0);
  SessionLockCallback m_lockCallback;
  SessionLockCallback m_unlockCallback;
  IdleInhibitCallback m_idleInhibitCallback;
  // Valid only in IdleInhibit::Held.
  int m_idleInhibitFd = -1;
  IdleInhibit m_idleInhibit = IdleInhibit::None;
};
