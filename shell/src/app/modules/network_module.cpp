#include "app/modules/network_module.h"

#include "core/log.h"
#include "ipc/ipc_events.h"
#include "dbus/network/iwd_secret_agent.h"
#include "dbus/network/iwd_service.h"
#include "dbus/network/network_manager_service.h"
#include "dbus/network/wpa_supplicant_service.h"
#include "dbus/system_bus.h"

#include <exception>

namespace {

  constexpr Logger kLog("app");

} // namespace

NetworkModule::NetworkModule(SystemBus& bus, Hooks hooks) : m_bus(bus), m_hooks(std::move(hooks)) {}

void NetworkModule::start(const Config& /*config*/) {
  std::unique_ptr<INetworkService> networkManager;
  try {
    networkManager = std::make_unique<NetworkManagerService>(m_bus);
  } catch (const std::exception& e) {
    kLog.warn("NetworkManager backend disabled: {}", e.what());
  }
  if (networkManager != nullptr) {
    m_service = std::make_unique<NetworkBackend>(std::move(networkManager), [this] { return makeStandalone(); });
    m_service->setChangeCallback([changed = m_hooks.changed](const NetworkState& state, NetworkChangeOrigin origin) {
      ipc_events::network(state);
      if (changed) {
        changed(state, origin);
      }
    });
    m_nmOwner = NameOwnerWatch::name(
        m_bus.connection(), std::string(NetworkManagerService::kBusName),
        [this](const std::string& /*name*/, const std::string& /*oldOwner*/, const std::string& newOwner) {
          m_service->setNetworkManagerOwned(!newOwner.empty());
        });
    m_hooks.started(*m_service);
  }

  try {
    m_secretAgent = std::make_unique<NetworkSecretAgent>(m_bus);
  } catch (const std::exception& e) {
    kLog.warn("network secret agent disabled: {}", e.what());
    m_secretAgent.reset();
  }
}

std::unique_ptr<INetworkService> NetworkModule::makeStandalone() {
  try {
    auto wpa = std::make_unique<WpaSupplicantService>(m_bus);
    kLog.info("network service active (wpa_supplicant)");
    return wpa;
  } catch (const std::exception& e) {
    kLog.info("wpa_supplicant unavailable ({}), trying iwd", e.what());
  }
  try {
    auto iwd = std::make_unique<IwdService>(m_bus);
    try {
      iwd->setSecretAgent(std::make_unique<IwdSecretAgent>(m_bus));
    } catch (const std::exception& e) {
      kLog.warn("iwd secret agent disabled: {}", e.what());
    }
    kLog.info("network service active (iwd)");
    return iwd;
  } catch (const std::exception& e) {
    kLog.info("iwd unavailable ({})", e.what());
  }
  return nullptr;
}

void NetworkModule::stop() {
  m_nmOwner = {};
  m_secretAgent.reset();
  m_service.reset();
}
