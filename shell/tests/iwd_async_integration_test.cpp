#include "dbus/network/iwd_service.h"
#include "dbus/system_bus.h"
#include "tests/test_check.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <sdbus-c++/sdbus-c++.h>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// A fake iwd served on this thread: a reply it holds can only come when the test sends it, so a synchronous call
// in IwdService waits out its D-Bus timeout instead of returning.
int main() {
  using OrderedNetwork = sdbus::Struct<sdbus::ObjectPath, std::int16_t>;
  using OrderedResult = sdbus::Result<std::vector<OrderedNetwork>>;
  auto connection = sdbus::createSessionBusConnection(sdbus::ServiceName{"net.connman.iwd"});
  auto root = sdbus::createObject(*connection, sdbus::ObjectPath{"/"});
  root->addObjectManager();
  const sdbus::ObjectPath stationPath{"/net/connman/iwd/0/3"};
  const sdbus::ObjectPath networkPath{"/net/connman/iwd/0/3/686f6d65_psk"};
  std::vector<OrderedResult> heldOrdered;
  std::vector<sdbus::Result<>> heldScans;
  int orderedCalls = 0;
  auto station = sdbus::createObject(*connection, stationPath);
  station->addVTable(
             sdbus::registerProperty("Name").withGetter([] { return std::string{"wlan0"}; }),
             sdbus::registerProperty("Powered").withGetter([] { return true; })
  )
      .forInterface("net.connman.iwd.Device");
  station
      ->addVTable(
          sdbus::registerProperty("State").withGetter([] { return std::string{"connected"}; }),
          sdbus::registerProperty("Scanning").withGetter([] { return false; }),
          sdbus::registerProperty("ConnectedNetwork").withGetter([&] { return networkPath; }),
          sdbus::registerMethod("GetOrderedNetworks").implementedAs([&](OrderedResult&& result) {
            ++orderedCalls;
            heldOrdered.push_back(std::move(result));
          }),
          sdbus::registerMethod("Scan").implementedAs([&](sdbus::Result<>&& result) {
            heldScans.push_back(std::move(result));
          })
      )
      .forInterface("net.connman.iwd.Station");
  station
      ->addVTable(sdbus::registerMethod("GetDiagnostics").implementedAs([] {
        return std::map<std::string, sdbus::Variant>{};
      }))
      .forInterface("net.connman.iwd.StationDiagnostic");
  auto network = sdbus::createObject(*connection, networkPath);
  network
      ->addVTable(
          sdbus::registerProperty("Name").withGetter([] { return std::string{"home"}; }),
          sdbus::registerProperty("Type").withGetter([] { return std::string{"psk"}; }),
          sdbus::registerProperty("Connected").withGetter([] { return true; })
      )
      .forInterface("net.connman.iwd.Network");

  SystemBus bus;
  auto pump = [&](const std::function<bool()>& ready, std::chrono::milliseconds limit = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
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

  std::unique_ptr<IwdService> service;
  TEST_CHECK(returnsAtOnce([&] { service = std::make_unique<IwdService>(bus); }));
  TEST_CHECK(pump([&] { return heldOrdered.size() == 1; }));
  TEST_CHECK(!service->hasStateSnapshot());

  // A scan and a property change while the first refresh waits: both coalesce into one more refresh.
  TEST_CHECK(returnsAtOnce([&] { service->requestScan(); }));
  TEST_CHECK(pump([&] { return heldScans.size() == 1; }));
  station->emitPropertiesChangedSignal("net.connman.iwd.Station", {sdbus::PropertyName{"Scanning"}});
  heldOrdered.front().returnResults(std::vector<OrderedNetwork>{{networkPath, std::int16_t{-8000}}});
  TEST_CHECK(pump([&] { return service->hasStateSnapshot() && heldOrdered.size() == 2; }));
  TEST_CHECK(service->accessPoints().size() == 1);
  TEST_CHECK(service->accessPoints().front().ssid == "home");
  const auto weak = service->accessPoints().front().strength;

  heldOrdered.back().returnResults(std::vector<OrderedNetwork>{{networkPath, std::int16_t{-3000}}});
  TEST_CHECK(pump([&] { return service->accessPoints().front().strength > weak; }));
  TEST_CHECK(service->state().ssid == "home");
  TEST_CHECK(service->state().connected);
  TEST_CHECK(!pump([&] { return orderedCalls > 2; }, 300ms));

  heldScans.front().returnResults();
  service.reset();
  return 0;
}
