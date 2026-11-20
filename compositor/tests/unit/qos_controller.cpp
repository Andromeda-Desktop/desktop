#include "core/systemd_units.h"
#include "qos/controller.h"
#include "qos/holds.h"

#include "check.h"

#include <wayland-server-core.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <unistd.h>

using andromeda::SystemdUnits;
using andromeda::qos::Controller;
using andromeda::qos::registry;
using andromeda::qos::Rule;
using andromeda::qos::Snapshot;
using andromeda::qos::Tier;

namespace {

  constexpr const char* kAppSlice = "/user.slice/user-1000.slice/user@1000.service/app.slice/";
  constexpr const char* kTerminal = "app-andromeda-dev.andromeda.Terminal-50.scope";
  constexpr const char* kShellScope = "app-andromeda-dev.andromeda.Terminal-3531s4.scope";
  constexpr const char* kEditor = "app-org.example.Editor-1.scope";

  class FakeUnits : public SystemdUnits {
  public:
    FakeUnits() : SystemdUnits(nullptr) {}
    void setProperties(const std::string& unit, const Properties& /*properties*/) override { touched.push_back(unit); }
    bool setFrozen(const std::string& unit, bool /*frozen*/, std::function<void(bool)> /*done*/) override {
      touched.push_back(unit);
      return true;
    }
    bool startScope(const std::string& unit, int pid, std::function<void(bool)> /*done*/, const ScopeOptions& /*options*/)
        override {
      started[pid] = unit;
      return true;
    }
    std::vector<std::string> touched;
    std::map<int, std::string> started;
  };

  void write(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << text;
  }

  // A /proc and /sys tree: the terminal window (pid 100) runs a shell (pid 200) in a ptyd shell scope; that shell
  // started a windowed app (pid 400) and a build (pid 201). The editor (pid 300) has its own scope.
  struct Fixture {
    Fixture() {
      root = std::filesystem::temp_directory_path() / std::format("qos-controller-{}", getpid());
      std::filesystem::remove_all(root);
      const auto process = [this](int pid, const std::string& unit, const std::string& children = {}) {
        write(root / std::format("proc/{}/cgroup", pid), std::format("0::{}{}\n", kAppSlice, unit));
        write(root / std::format("proc/{}/comm", pid), "app\n");
        write(root / std::format("proc/{}/stat", pid), std::format("{} (app) S 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 7\n", pid));
        write(root / std::format("proc/{}/task/{}/children", pid, pid), children);
        std::filesystem::create_directories(root / std::format("sys/fs/cgroup{}{}", kAppSlice, unit));
      };
      write(root / "proc/self/cgroup", std::format("0::{}andromeda.service\n", kAppSlice));
      process(100, kTerminal, "200 ");
      process(200, kShellScope, "201 400 ");
      process(201, kShellScope);
      process(400, kShellScope);
      process(300, kEditor);
      write(root / "power/BAT0/type", "Battery\n");
      write(root / "power/BAT0/status", "Discharging\n");
      setenv("ANDROMEDA_QOS_TEST_POWER_SUPPLY", (root / "power").c_str(), 1);
      setenv("ANDROMEDA_QOS_TEST_DELAYS_MS", "20,40", 1);
      registry().apps.clear();
      loop = wl_event_loop_create();
    }
    ~Fixture() {
      wl_event_loop_destroy(loop);
      std::filesystem::remove_all(root);
    }
    // Runs the loop for `ms`: idle evaluations and policy timers.
    void run(int ms) const {
      const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
      while (std::chrono::steady_clock::now() < end) {
        wl_event_loop_dispatch_idle(loop);
        wl_event_loop_dispatch(loop, 5);
      }
    }
    std::filesystem::path root;
    wl_event_loop* loop = nullptr;
  };

  Snapshot::Window window(pid_t pid, bool hidden, Rule rule = Rule::Normal) {
    return {.pid = pid, .hidden = hidden, .screenCast = false, .rule = rule, .adoptable = true, .thawShown = false, .appId = {}};
  }

} // namespace

ANDROMEDA_TEST(aHiddenTerminalAndItsShellJobsAreNeverThrottled) {
  Fixture fixture;
  FakeUnits units;
  Snapshot snapshot;
  snapshot.windows = {window(100, true, Rule::NeverThrottle), window(300, true)};
  Controller controller(fixture.loop, units, false, [&snapshot] { return snapshot; }, fixture.root.string());
  controller.schedule();
  fixture.run(150);
  CHECK(registry().apps.at(kTerminal).tier <= Tier::Visible);
  CHECK(!registry().apps.at(kTerminal).frozen);
  CHECK(!registry().apps.contains(kShellScope));
  CHECK(!std::ranges::contains(units.touched, std::string(kShellScope)));
  // Balanced on battery: the hidden editor drops to one core after both delays.
  CHECK(registry().apps.at(kEditor).tier == Tier::HiddenLong);
}

ANDROMEDA_TEST(aWindowedAppStartedFromATerminalShellGetsItsOwnScope) {
  Fixture fixture;
  FakeUnits units;
  Snapshot snapshot;
  snapshot.windows = {window(100, false, Rule::NeverThrottle), window(400, false)};
  Controller controller(fixture.loop, units, false, [&snapshot] { return snapshot; }, fixture.root.string());
  controller.schedule();
  fixture.run(10);
  CHECK(units.started.contains(400));
  CHECK(!units.started.contains(200));
  CHECK(!units.started.contains(201));
}

ANDROMEDA_TEST(focusRaisesAThrottledAppAtOnce) {
  Fixture fixture;
  FakeUnits units;
  Snapshot snapshot;
  snapshot.windows = {window(300, true)};
  Controller controller(fixture.loop, units, false, [&snapshot] { return snapshot; }, fixture.root.string());
  controller.schedule();
  fixture.run(150);
  CHECK(registry().apps.at(kEditor).tier == Tier::HiddenLong);
  snapshot.windows = {window(300, false)};
  controller.focus(300);
  wl_event_loop_dispatch_idle(fixture.loop);
  CHECK(registry().apps.at(kEditor).tier == Tier::Focus);
}

int main() { return RUN_TESTS(); }
