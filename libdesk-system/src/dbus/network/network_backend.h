#pragma once

#include "dbus/network/inetwork_service.h"

#include <cstdint>
#include <functional>
#include <memory>

// The one network service the shell shows (C13, K25): NetworkManager while its bus name has an owner, else a
// standalone wpa_supplicant or iwd. The choice is made again on every NetworkManager owner change, so a late or
// restarted NetworkManager replaces the standalone daemon. The pointer stays the same across every switch.
class NetworkBackend final : public INetworkService {
public:
  // Null when no standalone daemon is on the bus.
  using StandaloneFactory = std::function<std::unique_ptr<INetworkService>()>;
  // Pending: the NetworkManager owner is not known yet; Pending and None show the NetworkManager service (unavailable).
  enum class Active : std::uint8_t { Pending, NetworkManager, Standalone, None };

  NetworkBackend(std::unique_ptr<INetworkService> networkManager, StandaloneFactory standalone);
  ~NetworkBackend() override;

  // The NetworkManager bus name got (true) or lost (false) its owner; the first call ends Pending.
  void setNetworkManagerOwned(bool owned);
  [[nodiscard]] Active active() const noexcept { return m_active; }

  void setChangeCallback(ChangeCallback callback) override;
  void refresh() override { current().refresh(); }
  [[nodiscard]] bool available() const noexcept override { return current().available(); }
  [[nodiscard]] const NetworkState& state() const noexcept override { return current().state(); }
  [[nodiscard]] bool hasStateSnapshot() const noexcept override { return current().hasStateSnapshot(); }
  [[nodiscard]] const std::vector<AccessPointInfo>& accessPoints() const noexcept override {
    return current().accessPoints();
  }
  [[nodiscard]] const std::vector<VpnConnectionInfo>& vpnConnections() const noexcept override {
    return current().vpnConnections();
  }
  void requestScan() override { current().requestScan(); }
  bool activateAccessPoint(const AccessPointInfo& ap) override { return current().activateAccessPoint(ap); }
  bool activateAccessPoint(const AccessPointInfo& ap, const std::string& psk) override {
    return current().activateAccessPoint(ap, psk);
  }
  [[nodiscard]] bool supportsEnterprise() const noexcept override { return current().supportsEnterprise(); }
  bool activateEnterpriseAccessPoint(
      const AccessPointInfo& ap, const network_enterprise::EnterpriseCredentials& credentials
  ) override {
    return current().activateEnterpriseAccessPoint(ap, credentials);
  }
  bool activateVpnConnection(const VpnConnectionInfo& vpn) override { return current().activateVpnConnection(vpn); }
  bool deactivateVpnConnection(const VpnConnectionInfo& vpn) override {
    return current().deactivateVpnConnection(vpn);
  }
  [[nodiscard]] bool canActivateWiredConnection() const noexcept override {
    return current().canActivateWiredConnection();
  }
  bool activateWiredConnection() override { return current().activateWiredConnection(); }
  bool deactivateWiredConnection() override { return current().deactivateWiredConnection(); }
  [[nodiscard]] bool canActivateCellularConnection() const noexcept override {
    return current().canActivateCellularConnection();
  }
  bool activateCellularConnection() override { return current().activateCellularConnection(); }
  bool deactivateCellularConnection() override { return current().deactivateCellularConnection(); }
  void setWirelessEnabled(bool enabled, WirelessEnabledCompletion onComplete = {}) override {
    current().setWirelessEnabled(enabled, std::move(onComplete));
  }
  void disconnect() override { current().disconnect(); }
  void forgetSsid(const std::string& ssid) override { current().forgetSsid(ssid); }
  [[nodiscard]] bool hasSavedConnection(const std::string& ssid) const override {
    return current().hasSavedConnection(ssid);
  }

private:
  [[nodiscard]] INetworkService& current() const noexcept;
  void emitCurrent();

  std::unique_ptr<INetworkService> m_networkManager;
  StandaloneFactory m_makeStandalone;
  // Set only in Active::Standalone.
  std::unique_ptr<INetworkService> m_standalone;
  Active m_active = Active::Pending;
  ChangeCallback m_callback;
};
