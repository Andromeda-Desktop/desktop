#include "dbus/network/network_backend.h"
#include "tests/test_check.h"

#include <memory>
#include <optional>
#include <random>
#include <string>

namespace {

  int g_liveStandalones = 0;

  class FakeService final : public INetworkService {
  public:
    FakeService(std::string name, bool standalone) : m_standalone(standalone) {
      m_state.ssid = std::move(name);
      if (m_standalone) {
        ++g_liveStandalones;
      }
    }
    ~FakeService() override {
      if (m_standalone) {
        --g_liveStandalones;
      }
    }

    void emitChange() {
      if (m_callback) {
        m_callback(m_state, NetworkChangeOrigin::External);
      }
    }

    void setChangeCallback(ChangeCallback callback) override { m_callback = std::move(callback); }
    void refresh() override {}
    [[nodiscard]] const NetworkState& state() const noexcept override { return m_state; }
    [[nodiscard]] bool hasStateSnapshot() const noexcept override { return true; }
    [[nodiscard]] const std::vector<AccessPointInfo>& accessPoints() const noexcept override { return m_aps; }
    [[nodiscard]] const std::vector<VpnConnectionInfo>& vpnConnections() const noexcept override { return m_vpns; }
    void requestScan() override {}
    bool activateAccessPoint(const AccessPointInfo& /*ap*/) override { return false; }
    bool activateAccessPoint(const AccessPointInfo& /*ap*/, const std::string& /*psk*/) override { return false; }
    bool activateVpnConnection(const VpnConnectionInfo& /*vpn*/) override { return false; }
    bool deactivateVpnConnection(const VpnConnectionInfo& /*vpn*/) override { return false; }
    void setWirelessEnabled(bool /*enabled*/, WirelessEnabledCompletion /*onComplete*/) override {}
    void disconnect() override {}
    void forgetSsid(const std::string& /*ssid*/) override {}
    [[nodiscard]] bool hasSavedConnection(const std::string& /*ssid*/) const override { return false; }

  private:
    bool m_standalone;
    NetworkState m_state;
    std::vector<AccessPointInfo> m_aps;
    std::vector<VpnConnectionInfo> m_vpns;
    ChangeCallback m_callback;
  };

} // namespace

int main() {
  // Random event sequences against the model: the shown service is NetworkManager exactly while its name has an
  // owner, one standalone service lives exactly while it is shown, and only the shown service reaches the callback.
  for (unsigned seed = 1; seed <= 200; ++seed) {
    std::mt19937 rng(seed);
    auto nmOwned = std::optional<bool>{};
    bool standaloneOnBus = false;
    FakeService* standalone = nullptr;

    auto nmService = std::make_unique<FakeService>("nm", false);
    FakeService* nm = nmService.get();
    NetworkBackend backend(std::move(nmService), [&]() -> std::unique_ptr<INetworkService> {
      if (!standaloneOnBus) {
        return nullptr;
      }
      auto service = std::make_unique<FakeService>("wpa", true);
      standalone = service.get();
      return service;
    });
    std::string lastEmitted;
    int emits = 0;
    backend.setChangeCallback([&](const NetworkState& state, NetworkChangeOrigin /*origin*/) {
      lastEmitted = state.ssid;
      ++emits;
    });

    for (int step = 0; step < 60; ++step) {
      const int before = emits;
      switch (rng() % 5) {
      case 0:
        nmOwned = true;
        backend.setNetworkManagerOwned(true);
        break;
      case 1:
        nmOwned = false;
        backend.setNetworkManagerOwned(false);
        break;
      case 2:
        standaloneOnBus = !standaloneOnBus;
        break;
      case 3:
        nm->emitChange();
        TEST_CHECK((emits > before) == (backend.active() != NetworkBackend::Active::Standalone));
        break;
      default:
        if (g_liveStandalones == 1) {
          standalone->emitChange();
          TEST_CHECK(emits > before);
        }
        break;
      }

      using Active = NetworkBackend::Active;
      if (!nmOwned.has_value()) {
        TEST_CHECK(backend.active() == Active::Pending);
      } else if (*nmOwned) {
        TEST_CHECK(backend.active() == Active::NetworkManager);
      } else {
        TEST_CHECK(backend.active() == Active::Standalone || backend.active() == Active::None);
      }
      TEST_CHECK(g_liveStandalones == (backend.active() == Active::Standalone ? 1 : 0));
      TEST_CHECK(backend.state().ssid == (backend.active() == Active::Standalone ? "wpa" : "nm"));
      if (emits > before) {
        TEST_CHECK(lastEmitted == backend.state().ssid);
      }
    }
  }
  TEST_CHECK(g_liveStandalones == 0);
  return 0;
}
