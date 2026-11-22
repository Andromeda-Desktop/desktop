#include "core/timer_manager.h"
#include "dbus/logind/logind_service.h"
#include "dbus/system_bus.h"
#include "tests/test_check.h"

#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

  const sdbus::ServiceName kLogindBusName{"org.freedesktop.login1"};
  const sdbus::ObjectPath kLogindPath{"/org/freedesktop/login1"};
  constexpr auto kManagerInterface = "org.freedesktop.login1.Manager";

  // logind's Inhibit as LogindService sees it: each call returns a fresh fd and is counted.
  class FakeLogind {
  public:
    FakeLogind()
        : connection(sdbus::createSessionBusConnection(kLogindBusName)),
          manager(sdbus::createObject(*connection, kLogindPath)) {
      manager
          ->addVTable(
              sdbus::registerMethod("Inhibit")
                  .implementedAs([this](const std::string&, const std::string&, const std::string&, const std::string&) {
                    ++inhibits;
                    int fds[2];
                    TEST_CHECK(::pipe2(fds, O_CLOEXEC) == 0);
                    ::close(fds[1]);
                    return sdbus::UnixFd{fds[0], sdbus::adopt_fd};
                  })
          )
          .forInterface(kManagerInterface);
      connection->enterEventLoopAsync();
    }

    ~FakeLogind() { connection->leaveEventLoop(); }

    FakeLogind(const FakeLogind&) = delete;
    FakeLogind& operator=(const FakeLogind&) = delete;

    void dropBusName() { connection->releaseName(kLogindBusName); }

    std::unique_ptr<sdbus::IConnection> connection;
    std::unique_ptr<sdbus::IObject> manager;
    std::atomic<int> inhibits{0};
  };

  template <typename Predicate> void drainUntil(SystemBus& bus, Predicate&& predicate) {
    for (int attempt = 0; attempt < 2000; ++attempt) {
      bus.processPendingEvents();
      TimerManager::instance().tick();
      if (std::invoke(predicate)) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    noctalia::test::failCheck("timed out waiting for D-Bus state");
  }

  void settle(SystemBus& bus) {
    for (int i = 0; i < 100; ++i) {
      bus.processPendingEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  using SleepDelay = LogindService::SleepDelay;

  // logind restarts while the delay is held: the new logind gets an Inhibit, so the next suspend still waits for the
  // lock.
  void restartTakesTheDelayAgain() {
    FakeLogind first;
    SystemBus bus;
    LogindService logind(bus);
    logind.acquireSleepDelayInhibit();
    drainUntil(bus, [&] { return logind.sleepDelay() == SleepDelay::Held; });

    first.dropBusName();
    FakeLogind second;
    drainUntil(bus, [&] { return second.inhibits == 1 && logind.sleepDelay() == SleepDelay::Held; });

    logind.releaseSleepDelayInhibit();
    TEST_CHECK(logind.sleepDelay() == SleepDelay::None);
  }

  // Inhibit fails while logind is away; it is taken once logind starts.
  void failedInhibitIsTakenWhenLogindStarts() {
    SystemBus bus;
    LogindService logind(bus);
    logind.acquireSleepDelayInhibit();
    drainUntil(bus, [&] { return logind.sleepDelay() == SleepDelay::None; });

    FakeLogind started;
    drainUntil(bus, [&] { return started.inhibits == 1 && logind.sleepDelay() == SleepDelay::Held; });
    logind.releaseSleepDelayInhibit();
  }

  // A released delay stays released across a restart.
  void releasedDelayIsNotTakenAgain() {
    FakeLogind first;
    SystemBus bus;
    LogindService logind(bus);
    logind.acquireSleepDelayInhibit();
    drainUntil(bus, [&] { return logind.sleepDelay() == SleepDelay::Held; });
    logind.releaseSleepDelayInhibit();

    first.dropBusName();
    FakeLogind second;
    settle(bus);
    TEST_CHECK(second.inhibits == 0);
    TEST_CHECK(logind.sleepDelay() == SleepDelay::None);
  }

} // namespace

int main() {
  restartTakesTheDelayAgain();
  failedInhibitIsTakenWhenLogindStarts();
  releasedDelayIsNotTakenAgain();
  return 0;
}
