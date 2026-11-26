#include "dbus/network/wpa_supplicant_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"
#include "system/rfkill_helper.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string>
#include <vector>

namespace {

  constexpr Logger kLog("wpa_supplicant");

  const sdbus::ServiceName kWpaBusName{"fi.w1.wpa_supplicant1"};
  const sdbus::ObjectPath kWpaObjectPath{"/fi/w1/wpa_supplicant1"};
  constexpr auto kWpaInterface = "fi.w1.wpa_supplicant1";
  constexpr auto kWpaIfaceInterface = "fi.w1.wpa_supplicant1.Interface";
  constexpr auto kWpaBssInterface = "fi.w1.wpa_supplicant1.BSS";
  constexpr auto kWpaNetworkInterface = "fi.w1.wpa_supplicant1.Network";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

  constexpr auto kStateCompleted = "completed";
  constexpr auto kStateAssociated = "associated";
  constexpr auto kStateAssociating = "associating";
  constexpr auto kStateGroupHandshake = "group_handshake";
  constexpr auto k_state4wayHandshake = "4way_handshake";

  using VariantMap = std::map<std::string, sdbus::Variant>;

  template <typename T> T propOr(const VariantMap& props, const char* name, T fallback) {
    const auto it = props.find(name);
    if (it == props.end()) {
      return fallback;
    }
    try {
      return it->second.get<T>();
    } catch (const sdbus::Error&) {
      return fallback;
    }
  }

  std::uint8_t signalToPercent(std::int16_t dBm) {
    if (dBm <= -100)
      return 0;
    if (dBm >= -50)
      return 100;
    return static_cast<std::uint8_t>(2 * (dBm + 100));
  }

  // wpa_supplicant quotes D-Bus string values for "ssid" itself; a byte array is taken verbatim.
  sdbus::Variant ssidVariant(const std::string& ssid) {
    return sdbus::Variant{std::vector<std::uint8_t>{ssid.begin(), ssid.end()}};
  }

  struct BssInfo {
    std::string ssid;
    std::int16_t signal = -100;
    bool secured = false;
  };

  std::optional<BssInfo> bssInfo(const VariantMap& props) {
    const auto ssidBytes = propOr(props, "SSID", std::vector<std::uint8_t>{});
    if (ssidBytes.empty()) {
      return std::nullopt;
    }
    BssInfo info;
    info.ssid = {reinterpret_cast<const char*>(ssidBytes.data()), ssidBytes.size()};
    info.signal = propOr(props, "Signal", std::int16_t{-100});
    // Secured if RSN or WPA map is non-empty.
    info.secured = !propOr(props, "RSN", VariantMap{}).empty() || !propOr(props, "WPA", VariantMap{}).empty();
    return info;
  }

  // A saved network's ssid: wpa_supplicant reports it quoted.
  std::string networkSsid(const VariantMap& networkProps) {
    auto ssid = propOr(propOr(networkProps, "Properties", VariantMap{}), "ssid", std::string{});
    if (ssid.size() >= 2 && ssid.front() == '"' && ssid.back() == '"') {
      ssid = ssid.substr(1, ssid.size() - 2);
    }
    return ssid;
  }

  bool under(const std::string& path, const std::string& parent) {
    return path.size() > parent.size() && path.starts_with(parent) && path[parent.size()] == '/';
  }

} // namespace

WpaSupplicantService::WpaSupplicantService(SystemBus& bus) : m_bus(bus) {
  if (!bus.nameHasOwner("fi.w1.wpa_supplicant1")) {
    throw sdbus::Error(
        sdbus::Error::Name{"org.freedesktop.DBus.Error.ServiceUnknown"},
        "The name fi.w1.wpa_supplicant1 was not provided by any .service files"
    );
  }

  m_wpa = sdbus::createProxy(m_bus.connection(), kWpaBusName, kWpaObjectPath);

  m_wpa->uponSignal("InterfaceAdded")
      .onInterface(kWpaInterface)
      .call([this](const sdbus::ObjectPath& path, const VariantMap&) {
        subscribeInterface(std::string(path));
        rebuildState();
      });

  m_wpa->uponSignal("InterfaceRemoved").onInterface(kWpaInterface).call([this](const sdbus::ObjectPath& path) {
    const std::string ifacePath(path);
    m_interfaces.erase(ifacePath);
    std::erase_if(m_bsses, [&](const auto& entry) { return under(entry.first, ifacePath); });
    std::erase_if(m_networks, [&](const auto& entry) { return under(entry.first, ifacePath); });
    std::erase_if(m_savedNetworks, [&](const auto& entry) { return under(entry.second, ifacePath); });
    rebuildState();
  });

  // A wpa_supplicant restart makes new interface objects: forget the old ones. No call while it is gone, as a call
  // would start it again.
  m_wpaOwner = NameOwnerWatch::changes(
      m_bus.connection(), std::string(kWpaBusName),
      [this](const std::string& /*name*/, const std::string& /*oldOwner*/, const std::string& newOwner) {
        m_interfaces.clear();
        m_bsses.clear();
        m_networks.clear();
        m_savedNetworks.clear();
        if (newOwner.empty()) {
          m_accessPoints.clear();
          emitChangedIfNeeded(NetworkState{});
        } else {
          subscribeInterfaces();
        }
      });
  subscribeInterfaces();
}

void WpaSupplicantService::subscribeInterfaces() {
  try {
    m_wpa->getPropertyAsync("Interfaces")
        .onInterface(kWpaInterface)
        .uponReplyInvoke([this](std::optional<sdbus::Error> error, sdbus::Variant value) {
          if (error) {
            kLog.warn("failed to enumerate interfaces: {}", error->what());
            return;
          }
          try {
            for (const auto& path : value.get<std::vector<sdbus::ObjectPath>>()) {
              subscribeInterface(std::string(path));
            }
          } catch (const sdbus::Error& e) {
            kLog.warn("bad Interfaces property: {}", e.what());
          }
          rebuildState();
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("failed to enumerate interfaces: {}", e.what());
  }
}

WpaSupplicantService::~WpaSupplicantService() = default;

void WpaSupplicantService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void WpaSupplicantService::subscribeInterface(const std::string& ifacePath) {
  if (m_interfaces.contains(ifacePath))
    return;
  try {
    auto proxy = sdbus::createProxy(m_bus.connection(), kWpaBusName, sdbus::ObjectPath{ifacePath});

    proxy->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this, ifacePath](
                  const std::string& interface, const VariantMap& changed, const std::vector<std::string>& invalidated
              ) {
          const auto it = m_interfaces.find(ifacePath);
          if (interface != kWpaIfaceInterface || it == m_interfaces.end()) {
            return;
          }
          for (const auto& [name, value] : changed) {
            it->second.props.insert_or_assign(name, value);
          }
          if (!invalidated.empty()) {
            loadProperties(m_interfaces, ifacePath, kWpaIfaceInterface);
          }
          syncInterfaceObjects(ifacePath);
          rebuildState();
        });

    // A scan updates every BSS signal: read them again.
    proxy->uponSignal("ScanDone").onInterface(kWpaIfaceInterface).call([this, ifacePath](bool) {
      for (const auto& [bssPath, bss] : m_bsses) {
        (void)bss;
        if (under(bssPath, ifacePath)) {
          loadProperties(m_bsses, bssPath, kWpaBssInterface);
        }
      }
    });

    proxy->uponSignal("BSSAdded")
        .onInterface(kWpaIfaceInterface)
        .call([this](const sdbus::ObjectPath& bssPath, const VariantMap&) { trackBss(std::string(bssPath)); });

    proxy->uponSignal("BSSRemoved").onInterface(kWpaIfaceInterface).call([this](const sdbus::ObjectPath& bssPath) {
      m_bsses.erase(std::string(bssPath));
      rebuildState();
    });

    m_interfaces.emplace(ifacePath, CachedObject{.proxy = std::move(proxy), .props = {}});
    loadProperties(m_interfaces, ifacePath, kWpaIfaceInterface);
  } catch (const sdbus::Error& e) {
    kLog.warn("failed to subscribe interface {}: {}", ifacePath, e.what());
  }
}

void WpaSupplicantService::loadProperties(
    std::unordered_map<std::string, CachedObject>& objects, const std::string& path, const char* interface
) {
  const auto it = objects.find(path);
  if (it == objects.end()) {
    return;
  }
  try {
    it->second.proxy->callMethodAsync("GetAll")
        .onInterface(kPropertiesInterface)
        .withArguments(std::string{interface})
        .uponReplyInvoke([this, &objects, path, interface](std::optional<sdbus::Error> error, VariantMap props) {
          const auto entry = objects.find(path);
          if (entry == objects.end()) {
            return;
          }
          if (error) {
            kLog.debug("GetAll {} failed on {}: {}", interface, path, error->what());
            return;
          }
          entry->second.props = std::move(props);
          if (&objects == &m_interfaces) {
            syncInterfaceObjects(path);
          } else if (&objects == &m_networks) {
            if (const auto ssid = networkSsid(entry->second.props); !ssid.empty()) {
              m_savedNetworks[ssid] = path;
            }
          }
          rebuildState();
        });
  } catch (const sdbus::Error& e) {
    kLog.debug("GetAll {} failed on {}: {}", interface, path, e.what());
  }
}

// Tracks the BSS and network objects an interface lists, so their properties are cached before a rebuild needs them.
void WpaSupplicantService::syncInterfaceObjects(const std::string& ifacePath) {
  const auto it = m_interfaces.find(ifacePath);
  if (it == m_interfaces.end()) {
    return;
  }
  const VariantMap& props = it->second.props;
  for (const auto& bssPath : propOr(props, "BSSs", std::vector<sdbus::ObjectPath>{})) {
    trackBss(std::string(bssPath));
  }
  for (const auto& networkPath : propOr(props, "Networks", std::vector<sdbus::ObjectPath>{})) {
    trackNetwork(std::string(networkPath));
  }
  if (const auto current = std::string(propOr(props, "CurrentNetwork", sdbus::ObjectPath{"/"})); current != "/") {
    trackNetwork(current);
  }
}

void WpaSupplicantService::trackBss(const std::string& bssPath) {
  if (m_bsses.contains(bssPath)) {
    return;
  }
  try {
    m_bsses.emplace(
        bssPath, CachedObject{.proxy = sdbus::createProxy(m_bus.connection(), kWpaBusName, sdbus::ObjectPath{bssPath}), .props = {}}
    );
    loadProperties(m_bsses, bssPath, kWpaBssInterface);
  } catch (const sdbus::Error& e) {
    kLog.debug("failed to track BSS {}: {}", bssPath, e.what());
  }
}

void WpaSupplicantService::trackNetwork(const std::string& networkPath) {
  if (m_networks.contains(networkPath)) {
    return;
  }
  try {
    m_networks.emplace(
        networkPath,
        CachedObject{.proxy = sdbus::createProxy(m_bus.connection(), kWpaBusName, sdbus::ObjectPath{networkPath}), .props = {}}
    );
    loadProperties(m_networks, networkPath, kWpaNetworkInterface);
  } catch (const sdbus::Error& e) {
    kLog.debug("failed to track network {}: {}", networkPath, e.what());
  }
}

sdbus::IProxy* WpaSupplicantService::firstInterface() const {
  return m_interfaces.empty() ? nullptr : m_interfaces.begin()->second.proxy.get();
}

bool WpaSupplicantService::hasSavedConnection(const std::string& ssid) const { return m_savedNetworks.contains(ssid); }

bool WpaSupplicantService::addAndSelect(const VariantMap& args, const char* what) {
  auto* iface = firstInterface();
  if (iface == nullptr)
    return false;
  const std::string ifacePath(iface->getObjectPath());
  try {
    iface->callMethodAsync("AddNetwork")
        .onInterface(kWpaIfaceInterface)
        .withArguments(args)
        .uponReplyInvoke([this, ifacePath, what](std::optional<sdbus::Error> error, sdbus::ObjectPath netPath) {
          const auto it = m_interfaces.find(ifacePath);
          if (error || it == m_interfaces.end()) {
            kLog.warn("activateAccessPoint ({}) failed: {}", what, error ? error->what() : "interface gone");
            return;
          }
          try {
            it->second.proxy->callMethodAsync("SelectNetwork")
                .onInterface(kWpaIfaceInterface)
                .withArguments(netPath)
                .uponReplyInvoke([this, ifacePath, what](std::optional<sdbus::Error> selectError) {
                  const auto iface = m_interfaces.find(ifacePath);
                  if (selectError || iface == m_interfaces.end()) {
                    kLog.warn("SelectNetwork ({}) failed: {}", what, selectError ? selectError->what() : "interface gone");
                    return;
                  }
                  try {
                    iface->second.proxy->callMethodAsync("SaveConfig")
                        .onInterface(kWpaIfaceInterface)
                        .uponReplyInvoke([](std::optional<sdbus::Error> saveError) {
                          if (saveError) {
                            kLog.warn("SaveConfig failed: {}", saveError->what());
                          }
                        });
                  } catch (const sdbus::Error& e) {
                    kLog.warn("SaveConfig failed: {}", e.what());
                  }
                });
          } catch (const sdbus::Error& e) {
            kLog.warn("SelectNetwork ({}) failed: {}", what, e.what());
          }
        });
    return true;
  } catch (const sdbus::Error& e) {
    kLog.warn("activateAccessPoint ({}) failed: {}", what, e.what());
    return false;
  }
}

namespace {
  // Fire and forget: the proxy belongs to the service, so it outlives the call.
  void callAsync(sdbus::IProxy& proxy, const char* method, const sdbus::ObjectPath* argument = nullptr) {
    auto onReply = [method = std::string(method)](std::optional<sdbus::Error> error) {
      if (error) {
        kLog.warn("{} failed: {}", method, error->what());
      }
    };
    try {
      if (argument != nullptr) {
        proxy.callMethodAsync(method).onInterface(kWpaIfaceInterface).withArguments(*argument).uponReplyInvoke(onReply);
      } else {
        proxy.callMethodAsync(method).onInterface(kWpaIfaceInterface).uponReplyInvoke(onReply);
      }
    } catch (const sdbus::Error& e) {
      kLog.warn("{} failed: {}", method, e.what());
    }
  }
} // namespace

bool WpaSupplicantService::activateAccessPoint(const AccessPointInfo& ap) {
  auto* iface = firstInterface();
  if (iface == nullptr)
    return false;
  if (const auto it = m_savedNetworks.find(ap.ssid); it != m_savedNetworks.end()) {
    const sdbus::ObjectPath network{it->second};
    callAsync(*iface, "SelectNetwork", &network);
    return true;
  }
  VariantMap args;
  args["ssid"] = ssidVariant(ap.ssid);
  args["key_mgmt"] = sdbus::Variant{std::string{"NONE"}};
  return addAndSelect(args, "open");
}

bool WpaSupplicantService::activateAccessPoint(const AccessPointInfo& ap, const std::string& psk) {
  VariantMap args;
  args["ssid"] = ssidVariant(ap.ssid);
  args["psk"] = sdbus::Variant{psk}; // raw passphrase, no quotes
  return addAndSelect(args, "psk");
}

void WpaSupplicantService::disconnect() {
  if (auto* iface = firstInterface(); iface != nullptr) {
    callAsync(*iface, "Disconnect");
  }
}

void WpaSupplicantService::setWirelessEnabled(bool enabled, WirelessEnabledCompletion onComplete) {
  const bool softBlocked = !enabled;
  bool rfkillDone = false;
  for (const auto& [ifacePath, iface] : m_interfaces) {
    (void)ifacePath;
    const auto ifname = propOr(iface.props, "Ifname", std::string{});
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

  bool fallbackCommandSent = false;
  if (!rfkillDone) {
    if (auto* iface = firstInterface(); iface != nullptr) {
      callAsync(*iface, enabled ? "Reconnect" : "Disconnect");
      fallbackCommandSent = true;
    }
  }

  const bool success = rfkillDone || fallbackCommandSent;
  if (success) {
    m_wirelessEnabledOverride = enabled;
  }
  rebuildState();
  if (onComplete) {
    onComplete(success);
  }
}

void WpaSupplicantService::forgetSsid(const std::string& ssid) {
  auto* iface = firstInterface();
  if (iface == nullptr)
    return;
  const auto it = m_savedNetworks.find(ssid);
  if (it == m_savedNetworks.end())
    return;
  const sdbus::ObjectPath network{it->second};
  try {
    iface->callMethodAsync("RemoveNetwork")
        .onInterface(kWpaIfaceInterface)
        .withArguments(network)
        .uponReplyInvoke([iface](std::optional<sdbus::Error> error) {
          if (error) {
            kLog.warn("forgetSsid failed: {}", error->what());
            return;
          }
          callAsync(*iface, "SaveConfig");
        });
    m_networks.erase(it->second);
    m_savedNetworks.erase(it);
  } catch (const sdbus::Error& e) {
    kLog.warn("forgetSsid failed: {}", e.what());
  }
}

void WpaSupplicantService::rebuildState() {
  NetworkState next;
  next.wirelessEnabled = m_wirelessEnabledOverride.value_or(!m_interfaces.empty());

  std::vector<AccessPointInfo> aps;
  for (const auto& [ifacePath, iface] : m_interfaces) {
    const VariantMap& props = iface.props;
    const auto state = propOr(props, "State", std::string{"inactive"});
    next.scanning = next.scanning || propOr(props, "Scanning", false);

    const bool connected =
        (state == kStateCompleted
         || state == kStateAssociated
         || state == kStateGroupHandshake
         || state == k_state4wayHandshake);
    const bool associating = (state == kStateAssociating);
    const std::string activeBssPath =
        connected || associating ? std::string(propOr(props, "CurrentBSS", sdbus::ObjectPath{"/"})) : std::string{};

    if (connected || associating) {
      next.kind = NetworkConnectivity::Wireless;
      next.connected = connected;
      next.interfaceName = propOr(props, "Ifname", std::string{});
      const auto current = std::string(propOr(props, "CurrentNetwork", sdbus::ObjectPath{"/"}));
      if (const auto network = m_networks.find(current); network != m_networks.end()) {
        next.ssid = networkSsid(network->second.props);
      }
    }

    for (const auto& bssPath : propOr(props, "BSSs", std::vector<sdbus::ObjectPath>{})) {
      const std::string key(bssPath);
      const auto bss = m_bsses.find(key);
      if (bss == m_bsses.end()) {
        continue;
      }
      const auto info = bssInfo(bss->second.props);
      if (!info)
        continue;

      const bool active = (key == activeBssPath);
      const std::uint8_t pct = signalToPercent(info->signal);
      if (active) {
        next.signalStrength = pct;
        next.frequencyMhz = propOr(bss->second.props, "Frequency", std::uint16_t{0});
      }

      auto existing = std::ranges::find(aps, info->ssid, &AccessPointInfo::ssid);
      if (existing != aps.end()) {
        existing->strength = std::max(pct, existing->strength);
        if (active)
          existing->active = true;
      } else {
        AccessPointInfo ap;
        ap.path = key;
        ap.devicePath = ifacePath;
        ap.ssid = info->ssid;
        ap.strength = pct;
        ap.secured = info->secured;
        ap.active = active;
        aps.push_back(std::move(ap));
      }
    }
  }

  std::ranges::sort(aps, [](const AccessPointInfo& a, const AccessPointInfo& b) {
    if (a.active != b.active)
      return a.active > b.active;
    return a.strength > b.strength;
  });

  m_accessPoints = std::move(aps);
  emitChangedIfNeeded(std::move(next));
}

void WpaSupplicantService::refresh() {
  for (const auto& [ifacePath, iface] : m_interfaces) {
    (void)iface;
    loadProperties(m_interfaces, ifacePath, kWpaIfaceInterface);
  }
}

void WpaSupplicantService::requestScan() {
  const std::map<std::string, sdbus::Variant> args{{"Type", sdbus::Variant{std::string{"passive"}}}};
  for (const auto& [ifacePath, iface] : m_interfaces) {
    try {
      iface.proxy->callMethodAsync("Scan")
          .onInterface(kWpaIfaceInterface)
          .withArguments(args)
          .uponReplyInvoke([ifacePath](std::optional<sdbus::Error> error) {
            if (error) {
              kLog.debug("Scan failed on {}: {}", ifacePath, error->what());
            }
          });
    } catch (const sdbus::Error& e) {
      kLog.debug("Scan failed on {}: {}", ifacePath, e.what());
    }
  }
}

void WpaSupplicantService::emitChangedIfNeeded(NetworkState next) {
  const bool firstSnapshot = !m_hasStateSnapshot;
  const bool stateChanged = next != m_state;
  m_state = std::move(next);
  m_hasStateSnapshot = true;
  if ((firstSnapshot || stateChanged) && m_changeCallback) {
    m_changeCallback(m_state, NetworkChangeOrigin::External);
  }
}
