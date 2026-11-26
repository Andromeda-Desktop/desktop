#pragma once

#include "auth/fingerprint_auth_machine.h"
#include "core/timer_manager.h"
#include "dbus/name_owner_watch.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class SystemBus;

namespace sdbus {
  class IProxy;
}

class FingerprintAuthenticator {
public:
  using AuthenticatedCallback = std::function<void()>;
  using StatusCallback = std::function<void(const std::string& message, bool isError)>;
  using AvailabilityCallback = std::function<void()>;

  explicit FingerprintAuthenticator(SystemBus& bus);
  ~FingerprintAuthenticator();

  FingerprintAuthenticator(const FingerprintAuthenticator&) = delete;
  FingerprintAuthenticator& operator=(const FingerprintAuthenticator&) = delete;

  void setAuthenticatedCallback(AuthenticatedCallback callback);
  void setStatusCallback(StatusCallback callback);
  void setAvailabilityCallback(AvailabilityCallback callback) { m_onAvailability = std::move(callback); }
  // A finger is enrolled and the sensor listens now or after the running retry (false until fprintd answers).
  [[nodiscard]] bool available() const noexcept { return m_machine.available(); }

  void start();
  void stop();
  // The session goes to sleep (true) or resumes (false).
  void onSleep(bool sleeping);

private:
  void run(fingerprint_auth::Commands commands);
  void notifyAvailability();
  // Asks fprintd for its default device without blocking; on the reply it sets up m_device and claims it.
  void requestDevice(std::uint64_t attempt);
  bool createDeviceProxy(const std::string& path);
  void claimDevice(std::uint64_t attempt);
  void startVerify(std::uint64_t attempt);
  void stopVerify();
  void releaseDevice();
  void handleVerifyStatus(const std::string& result, bool done);
  void emitStatus(const std::string& message, bool isError);

  SystemBus& m_bus;
  NameOwnerWatch m_fprintOwner;
  std::unique_ptr<sdbus::IProxy> m_device;
  std::unique_ptr<sdbus::IProxy> m_manager; // alive while GetDefaultDevice is pending

  AuthenticatedCallback m_onAuthenticated;
  StatusCallback m_onStatus;

  Timer m_retryTimer;

  fingerprint_auth::Machine m_machine;
  // The available() value the callback last reported; only edge detection, never read as state.
  bool m_reportedAvailable = false;
  AvailabilityCallback m_onAvailability;
};
