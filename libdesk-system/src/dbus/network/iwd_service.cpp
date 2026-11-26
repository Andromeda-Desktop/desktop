#include "dbus/network/iwd_service.h"

#include "core/log.h"
#include "dbus/network/iwd_diagnostics.h"
#include "dbus/network/iwd_secret_agent.h"
#include "dbus/system_bus.h"
#include "system/rfkill_helper.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("iwd");

  const sdbus::ServiceName kIwdBusName{"net.connman.iwd"};
  const sdbus::ObjectPath kRootPath{"/"};
  constexpr auto kDeviceInterface = "net.connman.iwd.Device";
  constexpr auto kStationInterface = "net.connman.iwd.Station";
  constexpr auto kStationDiagnosticInterface = "net.connman.iwd.StationDiagnostic";
  constexpr auto kNetworkInterface = "net.connman.iwd.Network";
  constexpr auto kKnownNetworkInterface = "net.connman.iwd.KnownNetwork";
  constexpr auto kObjectManagerInterface = "org.freedesktop.DBus.ObjectManager";

  using VariantMap = std::map<std::string, sdbus::Variant>;
  using ObjectInterfaces = std::map<std::string, VariantMap>;
  using ManagedObjects = std::map<sdbus::ObjectPath, ObjectInterfaces>;
  using OrderedNetwork = sdbus::Struct<sdbus::ObjectPath, std::int16_t>;

  template <typename T> std::optional<T> variantGet(const sdbus::Variant& value) {
    try {
      return value.get<T>();
    } catch (const sdbus::Error&) {
      return std::nullopt;
    }
  }

  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

  std::uint8_t signalFromIwdStrength(std::int16_t centiDbm) { return iwd_diagnostics::signalToPercent(centiDbm / 100); }

  std::optional<std::string> stringProp(const VariantMap& props, std::string_view name) {
    const auto it = props.find(std::string{name});
    if (it == props.end()) {
      return std::nullopt;
    }
    return variantGet<std::string>(it->second);
  }

  std::optional<bool> boolProp(const VariantMap& props, std::string_view name) {
    const auto it = props.find(std::string{name});
    if (it == props.end()) {
      return std::nullopt;
    }
    return variantGet<bool>(it->second);
  }

  std::optional<sdbus::ObjectPath> objectPathProp(const VariantMap& props, std::string_view name) {
    const auto it = props.find(std::string{name});
    if (it == props.end()) {
      return std::nullopt;
    }
    return variantGet<sdbus::ObjectPath>(it->second);
  }

  bool isRootPath(const sdbus::ObjectPath& path) { return std::string(path) == "/"; }

} // namespace

IwdService::IwdService(SystemBus& bus) : m_bus(bus) {
  if (!bus.nameHasOwner("net.connman.iwd")) {
    throw sdbus::Error(
        sdbus::Error::Name{"org.freedesktop.DBus.Error.ServiceUnknown"},
        "The name net.connman.iwd was not provided by any .service files"
    );
  }

  m_iwd = sdbus::createProxy(m_bus.connection(), kIwdBusName, kRootPath);
  m_iwd->uponSignal("InterfacesAdded")
      .onInterface(kObjectManagerInterface)
      .call([this](const sdbus::ObjectPath&, const ObjectInterfaces&) { refresh(); });
  m_iwd->uponSignal("InterfacesRemoved")
      .onInterface(kObjectManagerInterface)
      .call([this](const sdbus::ObjectPath&, const std::vector<std::string>&) { refresh(); });
  // An iwd restart makes new objects: forget the old ones. No call while it is gone, as a call would start it again.
  m_iwdOwner = NameOwnerWatch::changes(
      m_bus.connection(), std::string(kIwdBusName),
      [this](const std::string& /*name*/, const std::string& /*oldOwner*/, const std::string& newOwner) {
        m_refreshRequest.cancel();
        m_refresh.reset();
        m_refreshQueued = false;
        m_objectProxies.clear();
        m_connectProxy.reset();
        m_knownNetworks.clear();
        m_deviceNames.clear();
        if (newOwner.empty()) {
          m_accessPoints.clear();
          emitChangedIfNeeded(NetworkState{});
        } else {
          refresh();
        }
      });

  refresh();
}

IwdService::~IwdService() = default;

void IwdService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void IwdService::setSecretAgent(std::unique_ptr<IwdSecretAgent> agent) {
  m_secretAgent = std::move(agent);
  if (m_secretAgent != nullptr) {
    m_secretAgent->setRequestCallback([this](const IwdSecretAgent::SecretRequest& request) {
      (void)request;
      if (m_pendingPsk.has_value()) {
        m_secretAgent->submitSecret(m_pendingPsk->text());
        m_pendingPsk.reset();
      } else {
        m_secretAgent->cancelSecret();
      }
    });
  }
}

void IwdService::subscribeObject(const std::string& path, const ObjectInterfaces& interfaces) {
  if (m_objectProxies.contains(path)) {
    return;
  }
  if (!interfaces.contains(kDeviceInterface)
      && !interfaces.contains(kStationInterface)
      && !interfaces.contains(kNetworkInterface)
      && !interfaces.contains(kKnownNetworkInterface)) {
    return;
  }

  try {
    auto proxy = sdbus::createProxy(m_bus.connection(), kIwdBusName, sdbus::ObjectPath{path});
    proxy->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this](const std::string& interfaceName, const VariantMap&, const std::vector<std::string>&) {
          if (interfaceName == kDeviceInterface
              || interfaceName == kStationInterface
              || interfaceName == kNetworkInterface
              || interfaceName == kKnownNetworkInterface) {
            refresh();
          }
        });
    m_objectProxies.emplace(path, std::move(proxy));
  } catch (const sdbus::Error& e) {
    kLog.debug("failed to subscribe IWD object {}: {}", path, e.what());
  }
}

struct IwdService::RefreshPass {
  ManagedObjects objects;
  std::map<sdbus::ObjectPath, std::vector<OrderedNetwork>> ordered;
  std::map<sdbus::ObjectPath, VariantMap> diagnostics;
  std::size_t pending = 0; // station replies still out
};

void IwdService::refresh() {
  if (m_refresh != nullptr) {
    m_refreshQueued = true;
    return;
  }
  m_refresh = std::make_unique<RefreshPass>();
  const LatestRequest::Ticket ticket = m_refreshRequest.begin();
  try {
    m_iwd->callMethodAsync("GetManagedObjects")
        .onInterface(kObjectManagerInterface)
        .uponReplyInvoke([this, ticket](std::optional<sdbus::Error> error, ManagedObjects objects) {
          if (!m_refreshRequest.current(ticket)) {
            return;
          }
          if (error) {
            kLog.warn("GetManagedObjects failed: {}", error->what());
            m_refresh.reset();
            return;
          }
          applyObjects(ticket, std::move(objects));
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("GetManagedObjects failed: {}", e.what());
    m_refresh.reset();
  }
}

void IwdService::applyObjects(LatestRequest::Ticket ticket, ManagedObjects objects) {
  RefreshPass& pass = *m_refresh;
  pass.objects = std::move(objects);
  m_knownNetworks.clear();
  m_deviceNames.clear();
  std::vector<std::string> liveObjects;
  for (const auto& [path, interfaces] : pass.objects) {
    const std::string pathString = std::string(path);
    subscribeObject(pathString, interfaces);
    liveObjects.push_back(pathString);

    if (const auto it = interfaces.find(kDeviceInterface); it != interfaces.end()) {
      const auto name = stringProp(it->second, "Name").value_or("");
      if (!name.empty()) {
        m_deviceNames[pathString] = name;
      }
    }
    if (const auto it = interfaces.find(kKnownNetworkInterface); it != interfaces.end()) {
      if (auto name = stringProp(it->second, "Name"); name.has_value() && !name->empty()) {
        m_knownNetworks[*name] = pathString;
      }
    }
  }
  for (auto it = m_objectProxies.begin(); it != m_objectProxies.end();) {
    if (std::ranges::find(liveObjects, it->first) == liveObjects.end()) {
      it = m_objectProxies.erase(it);
    } else {
      ++it;
    }
  }

  // Station replies land on m_objectProxies, which only an iwd restart clears, and that drops this pass too.
  const auto dispatch = [&](const sdbus::ObjectPath& stationPath, auto&& call) {
    const auto proxy = m_objectProxies.find(std::string(stationPath));
    if (proxy == m_objectProxies.end()) {
      kLog.debug("no proxy for IWD station {}", std::string(stationPath));
      return;
    }
    ++pass.pending;
    try {
      call(*proxy->second);
    } catch (const sdbus::Error& e) {
      --pass.pending;
      kLog.debug("station call failed on {}: {}", std::string(stationPath), e.what());
    }
  };
  for (const auto& [stationPath, interfaces] : pass.objects) {
    const auto stationIt = interfaces.find(kStationInterface);
    if (stationIt == interfaces.end()) {
      continue;
    }
    dispatch(stationPath, [&](sdbus::IProxy& proxy) {
      proxy.callMethodAsync("GetOrderedNetworks")
          .onInterface(kStationInterface)
          .uponReplyInvoke([this, ticket, stationPath](
                               std::optional<sdbus::Error> error, std::vector<OrderedNetwork> networks
                           ) {
            if (!m_refreshRequest.current(ticket)) {
              return;
            }
            if (error) {
              kLog.debug("GetOrderedNetworks failed on {}: {}", std::string(stationPath), error->what());
            } else {
              m_refresh->ordered[stationPath] = std::move(networks);
            }
            stationReplyDone();
          });
    });
    if (stringProp(stationIt->second, "State").value_or("") == "connected") {
      dispatch(stationPath, [&](sdbus::IProxy& proxy) {
        proxy.callMethodAsync("GetDiagnostics")
            .onInterface(kStationDiagnosticInterface)
            .uponReplyInvoke([this, ticket, stationPath](std::optional<sdbus::Error> error, VariantMap diagnostics) {
              if (!m_refreshRequest.current(ticket)) {
                return;
              }
              if (error) {
                kLog.debug("GetDiagnostics failed on {}: {}", std::string(stationPath), error->what());
              } else {
                m_refresh->diagnostics[stationPath] = std::move(diagnostics);
              }
              stationReplyDone();
            });
      });
    }
  }
  if (pass.pending == 0) {
    finishRefresh();
  }
}

void IwdService::stationReplyDone() {
  if (--m_refresh->pending == 0) {
    finishRefresh();
  }
}

void IwdService::finishRefresh() {
  const RefreshPass& pass = *m_refresh;
  NetworkState next;
  std::vector<AccessPointInfo> aps;
  for (const auto& [path, interfaces] : pass.objects) {
    if (const auto it = interfaces.find(kDeviceInterface); it != interfaces.end()) {
      next.wirelessEnabled = next.wirelessEnabled || boolProp(it->second, "Powered").value_or(true);
    }
  }

  for (const auto& [stationPath, interfaces] : pass.objects) {
    const auto stationIt = interfaces.find(kStationInterface);
    if (stationIt == interfaces.end()) {
      continue;
    }
    const auto state = stringProp(stationIt->second, "State").value_or("");
    const bool connected = state == "connected";
    const bool connecting = state == "connecting" || state == "associating" || state == "authenticating";
    next.scanning = next.scanning || boolProp(stationIt->second, "Scanning").value_or(false);

    std::string connectedNetworkPath;
    if (auto connectedNetwork = objectPathProp(stationIt->second, "ConnectedNetwork");
        connectedNetwork.has_value() && !isRootPath(*connectedNetwork)) {
      connectedNetworkPath = std::string(*connectedNetwork);
    }

    if (connected || connecting) {
      next.kind = NetworkConnectivity::Wireless;
      next.connected = connected;
      next.resolving = connecting;
      next.interfaceName = m_deviceNames[std::string(stationPath)];
    }

    static const std::vector<OrderedNetwork> kNoNetworks;
    const auto ordered = pass.ordered.find(stationPath);
    for (const auto& [networkPath, signal] : ordered != pass.ordered.end() ? ordered->second : kNoNetworks) {
      const auto objectIt = pass.objects.find(networkPath);
      if (objectIt == pass.objects.end()) {
        continue;
      }
      const auto networkIt = objectIt->second.find(kNetworkInterface);
      if (networkIt == objectIt->second.end()) {
        continue;
      }

      const auto ssid = stringProp(networkIt->second, "Name").value_or("");
      if (ssid.empty()) {
        continue;
      }
      const auto type = stringProp(networkIt->second, "Type").value_or("");
      const bool active = boolProp(networkIt->second, "Connected").value_or(false)
          || (!connectedNetworkPath.empty() && connectedNetworkPath == std::string(networkPath));
      const std::uint8_t strength = signalFromIwdStrength(signal);

      auto existing = std::ranges::find(aps, ssid, &AccessPointInfo::ssid);
      if (existing != aps.end()) {
        existing->strength = std::max(existing->strength, strength);
        existing->active = existing->active || active;
        existing->secured = existing->secured || type != "open";
      } else {
        AccessPointInfo ap;
        ap.path = std::string(networkPath);
        ap.devicePath = std::string(stationPath);
        ap.ssid = ssid;
        ap.strength = strength;
        ap.secured = type != "open";
        ap.active = active;
        aps.push_back(std::move(ap));
      }

      if (active) {
        next.ssid = ssid;
        next.signalStrength = std::max(next.signalStrength, strength);
      }
    }

    if (const auto diagnostics = pass.diagnostics.find(stationPath);
        connected && diagnostics != pass.diagnostics.end()) {
      iwd_diagnostics::applyStationDiagnostics(next, diagnostics->second);
    }
  }

  std::ranges::sort(aps, [](const AccessPointInfo& a, const AccessPointInfo& b) {
    if (a.active != b.active) {
      return a.active > b.active;
    }
    return a.strength > b.strength;
  });

  m_refresh.reset();
  m_accessPoints = std::move(aps);
  emitChangedIfNeeded(std::move(next));
  if (m_refreshQueued) {
    m_refreshQueued = false;
    refresh();
  }
}

void IwdService::callOnObject(const std::string& path, const char* interface, const char* method) {
  const auto proxy = m_objectProxies.find(path);
  if (proxy == m_objectProxies.end()) {
    kLog.debug("{} skipped: no proxy for IWD object {}", method, path);
    return;
  }
  try {
    proxy->second->callMethodAsync(method).onInterface(interface).uponReplyInvoke(
        [path, method = std::string(method)](std::optional<sdbus::Error> error) {
          if (error) {
            kLog.debug("{} failed on {}: {}", method, path, error->what());
          }
        }
    );
  } catch (const sdbus::Error& e) {
    kLog.debug("{} failed on {}: {}", method, path, e.what());
  }
}

void IwdService::requestScan() {
  for (const auto& [stationPath, ifname] : m_deviceNames) {
    (void)ifname;
    callOnObject(stationPath, kStationInterface, "Scan");
  }
  refresh();
}

bool IwdService::activateAccessPoint(const AccessPointInfo& ap) {
  try {
    // Starting a new connect supersedes any previous in-flight one.
    m_connectProxy = sdbus::createProxy(m_bus.connection(), kIwdBusName, sdbus::ObjectPath{ap.path});
    m_connectProxy->callMethodAsync("Connect")
        .onInterface(kNetworkInterface)
        .uponReplyInvoke([this, ssid = ap.ssid](std::optional<sdbus::Error> error) {
          // consumed by the agent, or abandoned, never keep it around
          m_pendingPsk.reset();
          if (error) {
            kLog.warn("Connect failed for {}: {}", ssid, error->what());
          }
          refresh();
        });
    // dispatched; real result arrives via the change callback
    return true;
  } catch (const sdbus::Error& e) {
    m_pendingPsk.reset();
    kLog.warn("Connect dispatch failed for {}: {}", ap.ssid, e.what());
    return false;
  }
}

bool IwdService::activateAccessPoint(const AccessPointInfo& ap, const std::string& psk) {
  if (psk.empty()) {
    return activateAccessPoint(ap);
  }
  if (m_secretAgent == nullptr) {
    kLog.warn("IWD secret agent not available; cannot connect to secured network");
    return false;
  }
  // Store the PSK for the agent to submit when RequestPassphrase is called
  m_pendingPsk.emplace(std::string_view(psk));
  return activateAccessPoint(ap);
}

void IwdService::setWirelessEnabled(bool enabled, WirelessEnabledCompletion onComplete) {
  const bool softBlocked = !enabled;
  bool rfkillDone = false;
  for (const auto& [stationPath, ifname] : m_deviceNames) {
    (void)stationPath;
    if (ifname.empty()) {
      continue;
    }
    const RfkillSwitchResult result = setRfkillSoftBlockedForNetInterface(ifname, softBlocked);
    if (result.hardBlocked) {
      kLog.warn("setWirelessEnabled: rfkill reports a hard block on {}", ifname);
    }
    if (result.success) {
      rfkillDone = true;
      break;
    }
  }

  if (!rfkillDone) {
    const RfkillSwitchResult fallback = setRfkillSoftBlocked(RfkillDeviceType::Wlan, softBlocked);
    if (fallback.hardBlocked) {
      kLog.warn("setWirelessEnabled: wlan rfkill reports a hard block");
    }
    if (fallback.success) {
      rfkillDone = true;
    } else if (!fallback.detail.empty()) {
      kLog.debug("setWirelessEnabled: wlan rfkill fallback: {}", fallback.detail);
    }
  }

  bool poweredUpdated = false;
  for (const auto& [stationPath, ifname] : m_deviceNames) {
    (void)ifname;
    const auto device = m_objectProxies.find(stationPath);
    if (device == m_objectProxies.end()) {
      continue;
    }
    try {
      device->second->setPropertyAsync("Powered")
          .onInterface(kDeviceInterface)
          .toValue(enabled)
          .uponReplyInvoke([path = stationPath](std::optional<sdbus::Error> error) {
            if (error) {
              kLog.debug("setting IWD Powered failed on {}: {}", path, error->what());
            }
          });
      poweredUpdated = true;
    } catch (const sdbus::Error& e) {
      kLog.debug("setting IWD Powered failed on {}: {}", stationPath, e.what());
    }
  }

  refresh();
  if (onComplete) {
    onComplete(rfkillDone || poweredUpdated);
  }
}

void IwdService::disconnect() {
  for (const auto& [stationPath, ifname] : m_deviceNames) {
    (void)ifname;
    callOnObject(stationPath, kStationInterface, "Disconnect");
  }
  refresh();
}

void IwdService::forgetSsid(const std::string& ssid) {
  const auto it = m_knownNetworks.find(ssid);
  if (it == m_knownNetworks.end()) {
    return;
  }
  callOnObject(it->second, kKnownNetworkInterface, "Forget");
  m_knownNetworks.erase(it);
  refresh();
}

bool IwdService::hasSavedConnection(const std::string& ssid) const { return m_knownNetworks.contains(ssid); }

void IwdService::emitChangedIfNeeded(NetworkState next) {
  const bool firstSnapshot = !m_hasStateSnapshot;
  const bool stateChanged = next != m_state;
  m_state = std::move(next);
  m_hasStateSnapshot = true;
  if ((firstSnapshot || stateChanged) && m_changeCallback) {
    m_changeCallback(m_state, NetworkChangeOrigin::External);
  }
}
