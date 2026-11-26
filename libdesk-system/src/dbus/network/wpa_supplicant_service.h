#pragma once

#include "dbus/name_owner_watch.h"
#include "dbus/network/inetwork_service.h"

#include <map>
#include <memory>
#include <sdbus-c++/Types.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class SystemBus;

namespace sdbus {
  class IProxy;
}

// Network backend for systems running wpa_supplicant without NetworkManager.
// Exposes the same INetworkService interface as NetworkManagerService.
class WpaSupplicantService : public INetworkService {
public:
  explicit WpaSupplicantService(SystemBus& bus);
  ~WpaSupplicantService() override;

  WpaSupplicantService(const WpaSupplicantService&) = delete;
  WpaSupplicantService& operator=(const WpaSupplicantService&) = delete;

  void setChangeCallback(ChangeCallback callback) override;
  void refresh() override;

  [[nodiscard]] const NetworkState& state() const noexcept override { return m_state; }
  [[nodiscard]] bool hasStateSnapshot() const noexcept override { return m_hasStateSnapshot; }
  [[nodiscard]] const std::vector<AccessPointInfo>& accessPoints() const noexcept override { return m_accessPoints; }
  [[nodiscard]] const std::vector<VpnConnectionInfo>& vpnConnections() const noexcept override {
    return m_vpnConnections;
  }

  void requestScan() override;

  bool activateAccessPoint(const AccessPointInfo& ap) override;
  bool activateAccessPoint(const AccessPointInfo& ap, const std::string& psk) override;
  bool activateVpnConnection(const VpnConnectionInfo& /*vpn*/) override { return false; }
  bool deactivateVpnConnection(const VpnConnectionInfo& /*vpn*/) override { return false; }
  void setWirelessEnabled(bool enabled, WirelessEnabledCompletion onComplete = {}) override;
  void disconnect() override;
  void forgetSsid(const std::string& ssid) override;
  [[nodiscard]] bool hasSavedConnection(const std::string& ssid) const override;

private:
  using VariantMap = std::map<std::string, sdbus::Variant>;
  // A wpa_supplicant object and its last known properties. Destroying the proxy cancels its pending replies.
  struct CachedObject {
    std::unique_ptr<sdbus::IProxy> proxy;
    VariantMap props;
  };

  void subscribeInterfaces();
  void subscribeInterface(const std::string& ifacePath);
  void loadProperties(std::unordered_map<std::string, CachedObject>& objects, const std::string& path, const char* interface);
  void syncInterfaceObjects(const std::string& ifacePath);
  void trackBss(const std::string& bssPath);
  void trackNetwork(const std::string& networkPath);
  void rebuildState();
  void emitChangedIfNeeded(NetworkState next);
  sdbus::IProxy* firstInterface() const;
  // AddNetwork, then SelectNetwork and SaveConfig, all async on the first interface.
  bool addAndSelect(const VariantMap& args, const char* what);

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_wpa;
  std::unordered_map<std::string, CachedObject> m_interfaces;
  std::unordered_map<std::string, CachedObject> m_bsses;
  // Network objects (saved networks) by path; their "Properties" map holds the ssid.
  std::unordered_map<std::string, CachedObject> m_networks;
  // ssid -> network object path.
  std::unordered_map<std::string, std::string> m_savedNetworks;
  NetworkState m_state;
  std::vector<AccessPointInfo> m_accessPoints;
  const std::vector<VpnConnectionInfo> m_vpnConnections; // always empty
  bool m_hasStateSnapshot = false;
  std::optional<bool> m_wirelessEnabledOverride;
  ChangeCallback m_changeCallback;
  NameOwnerWatch m_wpaOwner;
};
