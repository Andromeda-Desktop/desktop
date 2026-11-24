#include "dbus/name_owner_watch.h"
#include "tests/test_check.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <random>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <thread>
#include <vector>

// Model test on a private session bus: another thread takes and releases a name in a loop while the watch starts,
// so the GetNameOwner reply lands at random points among the signals. Invariants: the
// first report comes from "", the reports chain, only the first may have old == new, and the last report is the
// real owner. A namespace watch sees names under its prefix only.
namespace {
  struct Change {
    std::string name;
    std::string oldOwner;
    std::string newOwner;
  };

  void pump(sdbus::IConnection& bus) {
    while (bus.processPendingEvent()) {
    }
  }

  template <typename Ready> bool pumpUntil(sdbus::IConnection& bus, Ready ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      pump(bus);
      if (ready()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ready();
  }
} // namespace

int main() {
  const sdbus::ServiceName name{"org.andromeda.test.Owned"};
  auto watcher = sdbus::createSessionBusConnection();
  std::vector<std::unique_ptr<sdbus::IConnection>> owners;
  owners.push_back(sdbus::createSessionBusConnection());

  std::mt19937 rng(11);
  for (int run = 0; run < 200; ++run) {
    // A second thread toggles the name while the watch starts, so signals race the GetNameOwner reply.
    std::atomic<bool> stop{false};
    bool held = false;
    std::thread toggler([&] {
      while (!stop.load()) {
        if (held) {
          owners[0]->releaseName(name);
        } else {
          owners[0]->requestName(name);
        }
        held = !held;
      }
    });
    std::this_thread::sleep_for(std::chrono::microseconds(rng() % 300));
    std::vector<Change> reported;
    std::optional<NameOwnerWatch> watch =
        NameOwnerWatch::name(*watcher, name, [&](const auto& n, const auto& oldOwner, const auto& newOwner) {
          reported.push_back({n, oldOwner, newOwner});
        });
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(rng() % 2000);
    while (std::chrono::steady_clock::now() < until) {
      pump(*watcher);
    }
    stop = true;
    toggler.join();
    const std::string busOwner = held ? owners[0]->getUniqueName() : std::string();
    TEST_CHECK(pumpUntil(*watcher, [&] { return !reported.empty() && reported.back().newOwner == busOwner; }));
    pump(*watcher);
    std::string last;
    for (std::size_t i = 0; i < reported.size(); ++i) {
      TEST_CHECK(reported[i].name == name);
      TEST_CHECK(i == 0 || reported[i].oldOwner != reported[i].newOwner);
      TEST_CHECK(reported[i].oldOwner == last);
      last = reported[i].newOwner;
    }
    TEST_CHECK(last == busOwner);
    if (held) {
      owners[0]->releaseName(name);
    }
    watch.reset();
    pump(*watcher);
  }

  std::vector<std::string> seen;
  const NameOwnerWatch prefix = NameOwnerWatch::prefix(
      *watcher, "org.andromeda.test.Pre",
      [&](const std::string& n, const std::string& /*oldOwner*/, const std::string& /*newOwner*/) { seen.push_back(n); }
  );
  // The match is installed before requestName returns on the other connection: the daemon handles AddMatch first.
  owners[0]->requestName(sdbus::ServiceName{"org.andromeda.test.PreX"});
  owners[0]->requestName(sdbus::ServiceName{"org.andromeda.test.Pre.X"});
  TEST_CHECK(pumpUntil(*watcher, [&] { return !seen.empty(); }));
  pump(*watcher);
  TEST_CHECK(seen == std::vector<std::string>{"org.andromeda.test.Pre.X"});
  return 0;
}
