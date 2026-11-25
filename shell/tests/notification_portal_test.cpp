#include "dbus/notification/notification_portal.h"
#include "dbus/session_bus.h"
#include "notification/notification_manager.h"
#include "protocols/bus_names.h"
#include "tests/test_check.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {
  constexpr auto kInterface = "org.freedesktop.impl.portal.Notification";
  const sdbus::ObjectPath kPath{"/org/freedesktop/portal/desktop"};
  using Options = std::map<std::string, sdbus::Variant>;

  template <typename Predicate>
  bool drainUntil(SessionBus& bus, Predicate&& ready, std::chrono::milliseconds timeout = 5000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      bus.processPendingEvents();
      if (std::invoke(ready)) {
        return true;
      }
      std::this_thread::sleep_for(1ms);
    }
    bus.processPendingEvents();
    return std::invoke(ready);
  }
} // namespace

// Only the portal frontend may add or remove portal notifications, and only it hears their actions.
int main() {
  SessionBus bus;
  NotificationManager manager;
  NotificationPortal portal(bus, manager);
  const sdbus::ServiceName shell{bus.connection().getUniqueName()};

  auto frontend = sdbus::createSessionBusConnection(sdbus::ServiceName{busname::kPortalFrontend});
  auto intruder = sdbus::createSessionBusConnection();
  auto viaFrontend = sdbus::createProxy(*frontend, shell, kPath);
  auto viaIntruder = sdbus::createProxy(*intruder, shell, kPath);

  std::atomic<int> frontendActions{0};
  std::atomic<int> intruderActions{0};
  viaFrontend->uponSignal("ActionInvoked")
      .onInterface(kInterface)
      .call([&](const std::string&, const std::string&, const std::string&, const std::vector<sdbus::Variant>&) {
        ++frontendActions;
      });
  viaIntruder->uponSignal("ActionInvoked")
      .onInterface(kInterface)
      .call([&](const std::string&, const std::string&, const std::string&, const std::vector<sdbus::Variant>&) {
        ++intruderActions;
      });
  frontend->enterEventLoopAsync();
  intruder->enterEventLoopAsync();

  const auto call = [&](sdbus::IProxy& proxy, const char* method, auto... args) {
    std::atomic<bool> done{false};
    std::optional<sdbus::Error> failure;
    proxy.callMethodAsync(method).onInterface(kInterface).withArguments(args...).uponReplyInvoke(
        [&](std::optional<sdbus::Error> error) {
          failure = std::move(error);
          done = true;
        }
    );
    TEST_CHECK(drainUntil(bus, [&] { return done.load(); }));
    return failure;
  };
  const Options note{{"title", sdbus::Variant{std::string{"Hello"}}}};

  const auto denied = call(*viaIntruder, "AddNotification", std::string{"app.a"}, std::string{"n1"}, note);
  TEST_CHECK(denied.has_value() && denied->getName() == "org.freedesktop.DBus.Error.AccessDenied");
  TEST_CHECK(manager.all().empty());

  TEST_CHECK(!call(*viaFrontend, "AddNotification", std::string{"app.a"}, std::string{"n1"}, note).has_value());
  TEST_CHECK(manager.all().size() == 1);
  const std::uint32_t id = manager.all().front().id;

  TEST_CHECK(manager.invokeAction(id, "default", false));
  TEST_CHECK(drainUntil(bus, [&] { return frontendActions.load() == 1; }));

  // The reply comes after any signal sent to the intruder, on the same loop.
  TEST_CHECK(call(*viaIntruder, "RemoveNotification", std::string{"app.a"}, std::string{"n1"}).has_value());
  TEST_CHECK(intruderActions == 0);
  TEST_CHECK(manager.all().size() == 1);

  // A restarted frontend still hears the actions of notifications added before the restart.
  frontend->leaveEventLoop();
  viaFrontend.reset();
  frontend.reset();
  auto restarted = sdbus::createSessionBusConnection(sdbus::ServiceName{busname::kPortalFrontend});
  auto viaRestarted = sdbus::createProxy(*restarted, shell, kPath);
  std::atomic<int> restartedActions{0};
  viaRestarted->uponSignal("ActionInvoked")
      .onInterface(kInterface)
      .call([&](const std::string&, const std::string&, const std::string&, const std::vector<sdbus::Variant>&) {
        ++restartedActions;
      });
  restarted->enterEventLoopAsync();
  TEST_CHECK(manager.invokeAction(id, "default", false));
  TEST_CHECK(drainUntil(bus, [&] { return restartedActions.load() == 1; }));

  TEST_CHECK(!call(*viaRestarted, "RemoveNotification", std::string{"app.a"}, std::string{"n1"}).has_value());
  TEST_CHECK(manager.all().empty());

  restarted->leaveEventLoop();
  intruder->leaveEventLoop();
  return 0;
}
