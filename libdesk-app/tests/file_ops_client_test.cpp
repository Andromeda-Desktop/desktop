// Runs under dbus-run-session (meson.build): a private session bus with a fake org.andromeda.FileOps1.
#include "dbus/session_bus.h"
#include "files/file_ops_client.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <print>
#include <thread>

namespace {
  int g_failures = 0;

  void expect(bool ok, const char* what) {
    if (!ok) {
      std::println(stderr, "file_ops_client_test: {}", what);
      ++g_failures;
    }
  }

  template <typename F> bool waitFor(F done) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
      if (std::chrono::steady_clock::now() > until) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
  }

  constexpr auto kInterface = "org.andromeda.FileOps1";
  const sdbus::ObjectPath kJob{"/org/andromeda/FileOps1/job/1"};

  // The daemon side: Checksum starts a job that stays "running" until the test drops it.
  struct FakeDaemon {
    std::unique_ptr<sdbus::IConnection> connection = sdbus::createSessionBusConnection(sdbus::ServiceName{kInterface});
    std::unique_ptr<sdbus::IObject> manager = sdbus::createObject(*connection, sdbus::ObjectPath{"/org/andromeda/FileOps1"});
    std::unique_ptr<sdbus::IObject> job = sdbus::createObject(*connection, kJob);
    std::mutex lock;
    std::vector<sdbus::ObjectPath> jobs;

    FakeDaemon() {
      manager
          ->addVTable(
              sdbus::registerMethod("Checksum").implementedAs([this](const std::vector<std::uint8_t>&, const std::string&) {
                {
                  const std::scoped_lock guard(lock);
                  jobs = {kJob};
                }
                manager->emitPropertiesChangedSignal(kInterface, {sdbus::PropertyName{"Jobs"}});
                return kJob;
              }),
              sdbus::registerProperty("Jobs").withGetter([this] {
                const std::scoped_lock guard(lock);
                return jobs;
              })
          )
          .forInterface(kInterface);
      job->addVTable(sdbus::registerProperty("State").withGetter([] { return std::string("running"); }))
          .forInterface("org.andromeda.FileOps1.Job");
      connection->enterEventLoopAsync();
    }

    // A daemon restart: the job leaves the list without ever settling.
    void dropJob() {
      {
        const std::scoped_lock guard(lock);
        jobs.clear();
      }
      manager->emitPropertiesChangedSignal(kInterface, {sdbus::PropertyName{"Jobs"}});
    }
  };
} // namespace

int main() {
  FakeDaemon daemon;
  SessionBus bus;
  FileOpsClient client(bus);

  std::atomic<bool> started = false;
  std::atomic<bool> finished = false;
  std::mutex lock;
  std::string finishedError;
  client.checksum(
      "/tmp/file", "sha256", [&](const std::string& path, const std::string&) { started = !path.empty(); },
      [&](const std::string& hex, const std::string& error) {
        const std::scoped_lock guard(lock);
        finishedError = hex.empty() ? error : std::string();
        finished = true;
      }
  );
  bus.connection().enterEventLoopAsync();

  expect(waitFor([&] { return started.load(); }), "Checksum did not start a job");
  expect(waitFor([&] { return client.job(kJob) != nullptr; }), "the started job is not tracked");
  expect(!finished, "the waiter ran while the job still runs");

  daemon.dropJob();
  expect(waitFor([&] { return finished.load(); }), "a job that left the daemon before it settled never finished its waiter");
  {
    const std::scoped_lock guard(lock);
    expect(!finishedError.empty(), "the dropped job finished without an error");
  }

  bus.connection().leaveEventLoop();
  daemon.connection->leaveEventLoop();
  if (g_failures == 0) {
    std::println("file_ops_client_test: ok");
  }
  return g_failures == 0 ? 0 : 1;
}
