#include "dbus/logind/logind_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"

#include <cstdlib>
#include <fcntl.h>
#include <optional>
#include <sdbus-c++/Error.h>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string>
#include <unistd.h>
#include <utility>

namespace {
  constexpr Logger kLog("logind");

  const sdbus::ServiceName kLogindBusName{std::string(LogindService::kBusName)};
  const sdbus::ObjectPath kLogindObjectPath{"/org/freedesktop/login1"};
  constexpr auto kLogindManagerInterface = "org.freedesktop.login1.Manager";
  constexpr auto kLogindSessionInterface = "org.freedesktop.login1.Session";

  // Inhibit locks stay alive until every duplicate of the fd is closed. Our detached
  // process launcher double-forks without closing fds, so without CLOEXEC a launched app
  // inherits the idle inhibit and keeps it after the shell releases it.
  [[nodiscard]] bool setCloexec(int fd) {
    if (fd < 0) {
      return false;
    }
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0) {
      return false;
    }
    return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
  }
} // namespace

LogindService::LogindService(SystemBus& bus) : m_bus(bus) {
  m_managerProxy = sdbus::createProxy(m_bus.connection(), kLogindBusName, kLogindObjectPath);
}

LogindService::~LogindService() { releaseIdleInhibit(); }

void LogindService::ensureSessionLockMonitor() {
  if (m_sessionProxy != nullptr || m_sessionResolving) {
    return;
  }
  m_sessionResolving = true;

  const std::weak_ptr<int> aliveToken = m_aliveToken;
  const auto byPid = [this, aliveToken] {
    m_managerProxy->callMethodAsync("GetSessionByPID")
        .onInterface(kLogindManagerInterface)
        .withArguments(static_cast<std::uint32_t>(::getpid()))
        .uponReplyInvoke([this, aliveToken](std::optional<sdbus::Error> e, sdbus::ObjectPath path) {
          if (aliveToken.expired()) {
            return;
          }
          m_sessionResolving = false;
          if (e.has_value()) {
            kLog.warn("failed to resolve logind session: {}", e->what());
            kLog.warn("logind session lock monitor disabled: session path unavailable");
            m_pendingLockedHint.reset();
            return;
          }
          attachSessionProxy(path);
        });
  };

  const char* sessionId = std::getenv("XDG_SESSION_ID");
  if (sessionId == nullptr || sessionId[0] == '\0') {
    byPid();
    return;
  }
  m_managerProxy->callMethodAsync("GetSession")
      .onInterface(kLogindManagerInterface)
      .withArguments(std::string(sessionId))
      .uponReplyInvoke([this, aliveToken, byPid, id = std::string(sessionId)](
                           std::optional<sdbus::Error> e, sdbus::ObjectPath path
                       ) {
        if (aliveToken.expired()) {
          return;
        }
        if (e.has_value()) {
          kLog.debug("failed to resolve logind session via XDG_SESSION_ID={}: {}", id, e->what());
          byPid();
          return;
        }
        m_sessionResolving = false;
        attachSessionProxy(path);
      });
}

void LogindService::attachSessionProxy(const sdbus::ObjectPath& sessionPath) {
  if (!m_sessionLockIntegrationEnabled) {
    return;
  }
  m_sessionProxy = sdbus::createProxy(m_bus.connection(), kLogindBusName, sessionPath);
  m_sessionProxy->uponSignal("Lock").onInterface(kLogindSessionInterface).call([this] {
    if (m_lockCallback) {
      m_lockCallback();
    }
  });
  m_sessionProxy->uponSignal("Unlock").onInterface(kLogindSessionInterface).call([this] {
    if (!m_bus.isNameOwner(kLogindBusName, m_sessionProxy->getCurrentlyProcessedMessage().getSender())) {
      kLog.warn("ignoring Unlock signal not sent by logind");
      return;
    }
    if (m_unlockCallback) {
      m_unlockCallback();
    }
  });
  kLog.info("logind session lock monitor active ({})", std::string(sessionPath.c_str()));
  if (m_pendingLockedHint.has_value()) {
    setSessionLockedHint(*std::exchange(m_pendingLockedHint, std::nullopt));
  }
}

void LogindService::setSessionLockIntegrationEnabled(bool enabled) {
  if (m_sessionLockIntegrationEnabled == enabled) {
    return;
  }
  m_sessionLockIntegrationEnabled = enabled;
  if (!enabled) {
    m_sessionProxy.reset();
    m_pendingLockedHint.reset();
    kLog.info("logind session lock monitor disabled");
    return;
  }
  ensureSessionLockMonitor();
}

void LogindService::setLockCallback(SessionLockCallback callback) { m_lockCallback = std::move(callback); }

void LogindService::setUnlockCallback(SessionLockCallback callback) { m_unlockCallback = std::move(callback); }

void LogindService::setSessionLockedHint(bool locked) {
  if (!m_sessionLockIntegrationEnabled) {
    return;
  }
  if (m_sessionProxy == nullptr) {
    m_pendingLockedHint = locked;
    return;
  }
  m_sessionProxy->callMethodAsync("SetLockedHint")
      .onInterface(kLogindSessionInterface)
      .withArguments(locked)
      .uponReplyInvoke([locked](std::optional<sdbus::Error> e) {
        if (e.has_value()) {
          kLog.warn("failed to set logind session locked hint to {}: {}", locked, e->what());
        } else {
          kLog.debug("logind session locked hint set to {}", locked);
        }
      });
}

bool LogindService::supportsIdleInhibit() const noexcept { return m_managerProxy != nullptr; }

void LogindService::setIdleInhibitCallback(IdleInhibitCallback callback) {
  m_idleInhibitCallback = std::move(callback);
}

void LogindService::acquireIdleInhibit() {
  switch (m_idleInhibit) {
  case IdleInhibit::Held:
  case IdleInhibit::Pending:
    return;
  case IdleInhibit::PendingCancelled:
    m_idleInhibit = IdleInhibit::Pending;
    return;
  case IdleInhibit::None:
    break;
  }
  if (m_managerProxy == nullptr) {
    return;
  }
  m_idleInhibit = IdleInhibit::Pending;

  inhibit("idle", "noctalia", "Caffeine", "block", [this](ipc::Fd fd) {
    const bool cancelled = m_idleInhibit == IdleInhibit::PendingCancelled;
    m_idleInhibit = IdleInhibit::None;
    if (!fd || cancelled) {
      return;
    }
    m_idleInhibitFd = fd.release();
    m_idleInhibit = IdleInhibit::Held;
    kLog.info("logind idle inhibit acquired");
    if (m_idleInhibitCallback) {
      m_idleInhibitCallback();
    }
  });
}

void LogindService::inhibit(
    std::string what, std::string who, std::string why, std::string mode, InhibitCallback callback
) {
  const std::weak_ptr<int> aliveToken = m_aliveToken;
  m_managerProxy->callMethodAsync("Inhibit")
      .onInterface(kLogindManagerInterface)
      .withArguments(what, std::move(who), std::move(why), std::move(mode))
      .uponReplyInvoke([aliveToken, what, callback = std::move(callback)](
                           std::optional<sdbus::Error> e, sdbus::UnixFd reply
                       ) {
        if (aliveToken.expired()) {
          return;
        }
        ipc::Fd fd;
        if (e.has_value()) {
          kLog.warn("logind {} inhibit failed: {}", what, e->what());
        } else {
          fd = ipc::Fd(reply.release());
          if (!fd) {
            kLog.warn("logind {} inhibit returned no fd", what);
          } else if (!setCloexec(fd.get())) {
            kLog.warn("failed to set CLOEXEC on logind {} inhibit fd", what);
            fd = ipc::Fd();
          }
        }
        callback(std::move(fd));
      });
}

void LogindService::releaseIdleInhibit() {
  switch (m_idleInhibit) {
  case IdleInhibit::Pending:
    m_idleInhibit = IdleInhibit::PendingCancelled;
    return;
  case IdleInhibit::Held:
    ::close(m_idleInhibitFd);
    m_idleInhibitFd = -1;
    m_idleInhibit = IdleInhibit::None;
    kLog.debug("logind idle inhibit released");
    return;
  case IdleInhibit::None:
  case IdleInhibit::PendingCancelled:
    return;
  }
}
