// A StartPairing reply that arrives after cancelPairing() must not open the pairing window again.
#include "clipboard/clipboard_service.h"
#include "dbus/link/link_service.h"
#include "dbus/session_bus.h"
#include "notification/notification_manager.h"
#include "security/secret_store.h"
#include "security/storage_key_provider.h"
#include "tests/test_check.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <thread>
#include <tuple>

using namespace std::chrono_literals;

namespace {

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
    return false;
  }

} // namespace

int main() {
  auto daemon = sdbus::createSessionBusConnection(sdbus::ServiceName{"org.andromeda.Link1"});
  auto object = sdbus::createObject(*daemon, sdbus::ObjectPath{"/org/andromeda/Link1"});
  std::mutex mutex;
  std::optional<sdbus::Result<std::string, std::string>> heldStart;
  bool pairing = false;
  // The daemon answers StartPairing only when CancelPairing arrives, so the reply reaches the shell after its cancel.
  object
      ->addVTable(
          sdbus::registerMethod("StartPairing").implementedAs([&](sdbus::Result<std::string, std::string>&& result) {
            const std::scoped_lock lock(mutex);
            heldStart = std::move(result);
          }),
          sdbus::registerMethod("CancelPairing").implementedAs([&] {
            const std::scoped_lock lock(mutex);
            if (heldStart.has_value()) {
              heldStart->returnResults(std::string("123456"), std::string("andromeda-link://pair"));
              heldStart.reset();
            }
          }),
          sdbus::registerMethod("Ping").implementedAs([] {}),
          sdbus::registerProperty("Pairing").withGetter([&] { return pairing; })
      )
      .forInterface("org.andromeda.Link1");
  daemon->enterEventLoopAsync();

  SessionBus bus;
  NotificationManager notifications;
  security::SecretStore secrets;
  security::StorageKeyProvider keys(secrets);
  ClipboardService clipboard(keys);
  LinkService link(bus, notifications, clipboard, {});

  link.startPairing();
  TEST_CHECK(drainUntil(bus, [&] {
    const std::scoped_lock lock(mutex);
    return heldStart.has_value();
  }));
  link.cancelPairing();

  // Replies from one peer keep their order: Ping's reply comes after the late StartPairing reply.
  auto probe = sdbus::createProxy(bus.connection(), sdbus::ServiceName{"org.andromeda.Link1"},
                                  sdbus::ObjectPath{"/org/andromeda/Link1"});
  std::atomic<bool> pinged{false};
  probe->callMethodAsync("Ping").onInterface("org.andromeda.Link1").uponReplyInvoke(
      [&](std::optional<sdbus::Error> error) {
        TEST_CHECK(!error.has_value());
        pinged = true;
      });
  TEST_CHECK(drainUntil(bus, [&] { return pinged.load(); }));

  TEST_CHECK(!link.pairing().has_value());
  return 0;
}
