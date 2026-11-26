#include "dbus/network/wpa_supplicant_service.h"
#include "dbus/system_bus.h"
#include "tests/test_check.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// A fake wpa_supplicant served on this thread: it answers only while the test pumps it, so a synchronous call in
// WpaSupplicantService waits out its D-Bus timeout instead of returning.
int main() {
  using VariantMap = std::map<std::string, sdbus::Variant>;
  constexpr auto kIface = "fi.w1.wpa_supplicant1.Interface";
  auto connection = sdbus::createSessionBusConnection(sdbus::ServiceName{"fi.w1.wpa_supplicant1"});
  const sdbus::ObjectPath ifacePath{"/fi/w1/wpa_supplicant1/Interfaces/0"};
  const sdbus::ObjectPath bssPath{"/fi/w1/wpa_supplicant1/Interfaces/0/BSSs/1"};
  const sdbus::ObjectPath networkPath{"/fi/w1/wpa_supplicant1/Interfaces/0/Networks/0"};
  const sdbus::ObjectPath addedPath{"/fi/w1/wpa_supplicant1/Interfaces/0/Networks/1"};
  std::int16_t signal = -60;
  std::vector<std::string> calls;
  std::vector<sdbus::Result<>> heldScans;

  auto wpa = sdbus::createObject(*connection, sdbus::ObjectPath{"/fi/w1/wpa_supplicant1"});
  wpa->addVTable(sdbus::registerProperty("Interfaces").withGetter([&] { return std::vector{ifacePath}; }))
      .forInterface("fi.w1.wpa_supplicant1");
  auto iface = sdbus::createObject(*connection, ifacePath);
  iface
      ->addVTable(
          sdbus::registerProperty("State").withGetter([] { return std::string{"completed"}; }),
          sdbus::registerProperty("Ifname").withGetter([] { return std::string{"wlan0"}; }),
          sdbus::registerProperty("Scanning").withGetter([] { return false; }),
          sdbus::registerProperty("BSSs").withGetter([&] { return std::vector{bssPath}; }),
          sdbus::registerProperty("Networks").withGetter([&] { return std::vector{networkPath}; }),
          sdbus::registerProperty("CurrentBSS").withGetter([&] { return bssPath; }),
          sdbus::registerProperty("CurrentNetwork").withGetter([&] { return networkPath; }),
          sdbus::registerMethod("Scan").implementedAs([&](sdbus::Result<>&& result, const VariantMap&) {
            heldScans.push_back(std::move(result));
          }),
          sdbus::registerMethod("AddNetwork").implementedAs([&](const VariantMap&) {
            calls.emplace_back("AddNetwork");
            return addedPath;
          }),
          sdbus::registerMethod("SelectNetwork").implementedAs([&](const sdbus::ObjectPath& path) {
            calls.push_back("SelectNetwork " + std::string(path));
          }),
          sdbus::registerMethod("RemoveNetwork").implementedAs([&](const sdbus::ObjectPath& path) {
            calls.push_back("RemoveNetwork " + std::string(path));
          }),
          sdbus::registerMethod("SaveConfig").implementedAs([&] { calls.emplace_back("SaveConfig"); }),
          sdbus::registerSignal("ScanDone").withParameters<bool>()
      )
      .forInterface(kIface);
  auto bss = sdbus::createObject(*connection, bssPath);
  bss->addVTable(
         sdbus::registerProperty("SSID").withGetter([] { return std::vector<std::uint8_t>{'h', 'o', 'm', 'e'}; }),
         sdbus::registerProperty("Signal").withGetter([&] { return signal; }),
         sdbus::registerProperty("Frequency").withGetter([] { return std::uint16_t{5180}; }),
         sdbus::registerProperty("RSN").withGetter([] { return VariantMap{{"KeyMgmt", sdbus::Variant{std::string{"psk"}}}}; }),
         sdbus::registerProperty("WPA").withGetter([] { return VariantMap{}; })
  )
      .forInterface("fi.w1.wpa_supplicant1.BSS");
  auto network = sdbus::createObject(*connection, networkPath);
  network
      ->addVTable(sdbus::registerProperty("Properties").withGetter([] {
        return VariantMap{{"ssid", sdbus::Variant{std::string{"\"home\""}}}};
      }))
      .forInterface("fi.w1.wpa_supplicant1.Network");

  SystemBus bus;
  auto pump = [&](const std::function<bool()>& ready) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!ready() && std::chrono::steady_clock::now() < deadline) {
      bus.processPendingEvents();
      while (connection->processPendingEvent()) {
      }
      std::this_thread::sleep_for(1ms);
    }
    return ready();
  };
  auto returnsAtOnce = [](const std::function<void()>& call) {
    const auto start = std::chrono::steady_clock::now();
    call();
    return std::chrono::steady_clock::now() - start < 1s;
  };

  std::unique_ptr<WpaSupplicantService> service;
  TEST_CHECK(returnsAtOnce([&] { service = std::make_unique<WpaSupplicantService>(bus); }));
  TEST_CHECK(pump([&] { return service->state().ssid == "home" && service->accessPoints().size() == 1; }));
  TEST_CHECK(service->state().connected);
  TEST_CHECK(service->state().interfaceName == "wlan0");
  TEST_CHECK(service->state().frequencyMhz == 5180);
  TEST_CHECK(service->accessPoints().front().secured);
  TEST_CHECK(service->hasSavedConnection("home"));
  const auto weak = service->state().signalStrength;

  // A scan result reaches the cached BSS: ScanDone reads its properties again.
  signal = -52;
  iface->emitSignal("ScanDone").onInterface(kIface).withArguments(true);
  TEST_CHECK(pump([&] { return service->state().signalStrength > weak; }));

  TEST_CHECK(returnsAtOnce([&] { service->requestScan(); }));
  TEST_CHECK(pump([&] { return heldScans.size() == 1; }));

  AccessPointInfo cafe;
  cafe.ssid = "cafe";
  TEST_CHECK(returnsAtOnce([&] { TEST_CHECK(service->activateAccessPoint(cafe, "secret12")); }));
  TEST_CHECK(pump([&] { return calls.size() == 3; }));
  TEST_CHECK(calls[0] == "AddNetwork");
  TEST_CHECK(calls[1] == "SelectNetwork " + std::string(addedPath));
  TEST_CHECK(calls[2] == "SaveConfig");

  calls.clear();
  TEST_CHECK(returnsAtOnce([&] { service->forgetSsid("home"); }));
  TEST_CHECK(!service->hasSavedConnection("home"));
  TEST_CHECK(pump([&] { return calls.size() == 2; }));
  TEST_CHECK(calls[0] == "RemoveNetwork " + std::string(networkPath));
  TEST_CHECK(calls[1] == "SaveConfig");

  heldScans.front().returnResults();
  service.reset();
  return 0;
}
