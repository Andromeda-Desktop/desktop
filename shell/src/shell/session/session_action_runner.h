#pragma once

#include "config/config_types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

class CompositorPlatform;
class LockScreen;

struct SessionActionHooks {
  using AsyncHook = std::function<void(std::function<void(bool ok)> done)>;
  AsyncHook onLogout;
  AsyncHook onReboot;
  AsyncHook onShutdown;
  // Armed before Noctalia-initiated suspend so PrepareForSleep can skip lock-before-sleep.
  std::function<void()> onBeforePlainSuspend;
  std::function<void()> onPlainSuspendAborted;
};

class SessionActionRunner {
public:
  explicit SessionActionRunner(CompositorPlatform& platform, LockScreen& lockScreen, SessionActionHooks hooks = {});

  void setHooks(SessionActionHooks hooks);
  void setPowerConfig(const ShellSessionConfig::ShellSessionPowerConfig& power);
  // `failed` runs on the main loop when the action could not start or its command exited non-zero.
  void invoke(const SessionPanelActionConfig& cfg, std::function<void()> failed = {}) const;
  [[nodiscard]] bool lock() const;
  [[nodiscard]] bool requestSuspendDetached() const;
  [[nodiscard]] bool requestRebootDetached() const;
  [[nodiscard]] bool requestRebootUefiDetached() const;
  [[nodiscard]] bool requestShutdownDetached() const;
  // "suspend", "reboot" or "shutdown" past other apps' block inhibitors (logind *WithFlags with
  // SD_LOGIND_SKIP_INHIBITORS); polkit may refuse it without admin rights.
  [[nodiscard]] bool requestPowerSkippingInhibitors(std::string_view action) const;
  [[nodiscard]] bool requestHibernateDetached() const;
  [[nodiscard]] bool requestSuspendThenHibernateDetached() const;
  [[nodiscard]] bool lockThenSuspendDetached() const;

private:
  struct PowerCall {
    // Runs on any thread: it holds copies and never touches the runner.
    std::function<bool()> call;
    // A plain suspend arms the lock-before-sleep skip on the main loop first.
    bool plainSuspend = false;
  };
  enum class PowerRun : std::uint8_t { Idle, Running };

  [[nodiscard]] PowerCall powerCall(std::string_view action) const;
  [[nodiscard]] bool runNow(const PowerCall& power) const;
  // Hooks first, then the call on a worker thread; refused while another one runs.
  void runQueued(
      SessionActionHooks::AsyncHook hook, PowerCall power, std::string_view action, std::function<void()> failed
  ) const;
  [[nodiscard]] SessionActionHooks::AsyncHook hookFor(std::string_view action) const;

  CompositorPlatform& m_platform;
  LockScreen& m_lockScreen;
  SessionActionHooks m_hooks;
  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);
  // Main loop only.
  mutable PowerRun m_powerRun = PowerRun::Idle;

  std::optional<std::string> m_suspendCommandOverride;
  std::optional<std::string> m_rebootCommandOverride;
  std::optional<std::string> m_shutdownCommandOverride;
  std::optional<std::string> m_hibernateCommandOverride;
  std::optional<std::string> m_suspendThenHibernateCommandOverride;
};
