#include "dbus/network/network_backend.h"

#include "core/log.h"

#include <utility>

namespace {
  constexpr Logger kLog("network");
} // namespace

NetworkBackend::NetworkBackend(std::unique_ptr<INetworkService> networkManager, StandaloneFactory standalone)
    : m_networkManager(std::move(networkManager)), m_makeStandalone(std::move(standalone)) {
  m_networkManager->setChangeCallback([this](const NetworkState& state, NetworkChangeOrigin origin) {
    if (m_active != Active::Standalone && m_callback) {
      m_callback(state, origin);
    }
  });
}

NetworkBackend::~NetworkBackend() = default;

void NetworkBackend::setChangeCallback(ChangeCallback callback) { m_callback = std::move(callback); }

INetworkService& NetworkBackend::current() const noexcept {
  return m_active == Active::Standalone ? *m_standalone : *m_networkManager;
}

void NetworkBackend::setNetworkManagerOwned(bool owned) {
  if (owned) {
    if (m_active == Active::NetworkManager) {
      return;
    }
    if (m_active == Active::Standalone) {
      kLog.info("NetworkManager appeared; it replaces the standalone network service");
    }
    m_standalone.reset();
    m_active = Active::NetworkManager;
    kLog.info("network service active (NetworkManager)");
    emitCurrent();
    return;
  }
  if (m_active == Active::Standalone || m_active == Active::None) {
    return;
  }
  m_standalone = m_makeStandalone();
  if (m_standalone == nullptr) {
    m_active = Active::None;
    kLog.info("no network service on the bus; waiting for NetworkManager");
    emitCurrent();
    return;
  }
  m_active = Active::Standalone;
  m_standalone->setChangeCallback([this](const NetworkState& state, NetworkChangeOrigin origin) {
    if (m_active == Active::Standalone && m_callback) {
      m_callback(state, origin);
    }
  });
  emitCurrent();
}

void NetworkBackend::emitCurrent() {
  if (m_callback) {
    m_callback(current().state(), NetworkChangeOrigin::External);
  }
}
