#include "system/activity_service.h"
#include "tests/test_check.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

  const std::string kUser = "/user.slice/user-1000.slice/user@1000.service";

  void writeCgroup(const fs::path& root, const std::string& group, std::uint64_t cpuUsec, std::uint64_t memory, double memPressure = 0.0) {
    const fs::path dir = root / group.substr(1);
    fs::create_directories(dir);
    std::ofstream(dir / "cpu.stat") << "usage_usec " << cpuUsec << "\nuser_usec 1\nsystem_usec 2\n";
    const std::uint64_t pageCache = 1000ULL * 1024 * 1024;
    std::ofstream(dir / "memory.current") << memory + pageCache << "\n";
    std::ofstream(dir / "memory.stat") << "anon " << memory - 3 * 4096 << "\nfile " << pageCache + 4096 << "\nkernel " << 3 * 4096
                                       << "\nshmem " << 4096 << "\nslab_reclaimable " << 4096 << "\n";
    std::ofstream(dir / "io.stat") << "259:0 rbytes=" << cpuUsec << " wbytes=10 rios=1 wios=1 dbytes=0 dios=0\n";
    std::ofstream(dir / "memory.pressure") << "some avg10=" << memPressure << " avg60=0.00 avg300=0.00 total=1\nfull avg10=0.00 avg60=0.00 avg300=0.00 total=0\n";
  }

  SystemdUnit unit(const std::string& name, const std::string& appId, const std::string& slice, std::uint64_t started) {
    SystemdUnit u;
    u.name = name;
    u.appId = appId;
    u.slice = slice;
    u.controlGroup = kUser + "/" + slice + "/" + name;
    u.activeEnterUsec = started;
    return u;
  }

  const ActivityRow* row(const ActivityService& service, const std::string& key) {
    for (const auto& r : service.rows()) {
      if (r.key == key) {
        return &r;
      }
    }
    return nullptr;
  }

} // namespace

int main() {
  TEST_CHECK(unitFromProcCgroup("0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-foot@x.service/sub\n") == "app-foot@x.service");
  TEST_CHECK(unitFromProcCgroup("0::/user.slice/user-1000.slice/session-2.scope\n") == "session-2.scope");
  TEST_CHECK(unitFromProcCgroup("0::/\n").empty());

  char tmpl[] = "/tmp/activity-grouping-test-XXXXXX";
  const fs::path root = ::mkdtemp(tmpl);
  const std::uint64_t mb = 1024 * 1024;

  std::vector<SystemdUnit> units{
      unit("app-zen\\x2dbrowser@a.service", "zen-browser", "app.slice", 300),
      unit("app-zen\\x2dbrowser@b.service", "zen-browser", "app.slice", 200),
      unit("opencode.service", "", "app.slice", 100),
      unit("app-andromeda-1835-17.scope", "", "app.slice", 400),
      unit("pipewire.service", "", "session.slice", 50),
      unit("app-andromeda-launch\\x2dfirst.sh-46.scope", "launch-first.sh", "app.slice", 500),
      unit("app-andromeda-dev.andromeda.Terminal-7.scope", "dev.andromeda.Terminal", "app.slice", 600),
  };
  for (const auto& u : units) {
    writeCgroup(root, u.controlGroup, 0, 10 * mb);
  }
  writeCgroup(root, units[2].controlGroup, 0, 480 * mb, 4.0);

  // A windowless unit whose first process a windowed unit started joins that unit's row.
  const std::string launcher = unitForPid(::getppid());
  units.push_back(unit("app-andromeda-leftover-9.scope", "", "app.slice", 700));
  writeCgroup(root, units.back().controlGroup, 0, 10 * mb);
  std::ofstream(root / units.back().controlGroup.substr(1) / "cgroup.procs") << ::getpid() << "\n";

  ActivityService service(root.string(), 4);
  service.setUnits(units);
  service.setWindowUnits({{"app-zen\\x2dbrowser@a.service", "zen-browser"}, {"app-andromeda-1835-17.scope", "foot"},
                          {"app-andromeda-launch\\x2dfirst.sh-46.scope", "dev.andromeda.Terminal"},
                          {launcher, "launcher-app"}});
  const auto t0 = std::chrono::steady_clock::now();
  service.sample(t0);

  TEST_CHECK(service.rows().size() == 6);
  // A window launched through a wrapper script joins its app's row, with the app's windowless units.
  const ActivityRow* terminal = row(service, "dev.andromeda.Terminal");
  TEST_CHECK(terminal != nullptr && terminal->hasWindow && terminal->units.size() == 2 && terminal->memoryBytes == 20 * mb);
  TEST_CHECK(row(service, "launch-first.sh") == nullptr);
  TEST_CHECK(row(service, "app-andromeda-leftover-9.scope") == nullptr);
  TEST_CHECK(launcher.empty() || row(service, "launcher-app")->units.front() == "app-andromeda-leftover-9.scope");
  const ActivityRow* zen = row(service, "zen-browser");
  TEST_CHECK(zen != nullptr && zen->units.size() == 2 && zen->hasWindow && zen->memoryBytes == 20 * mb);
  TEST_CHECK(zen->startedUsec == 200);
  const ActivityRow* foot = row(service, "foot");
  TEST_CHECK(foot != nullptr && foot->appId == "foot" && foot->hasWindow && foot->units.front() == "app-andromeda-1835-17.scope");
  const ActivityRow* opencode = row(service, "opencode.service");
  TEST_CHECK(opencode != nullptr && !opencode->hasWindow && opencode->appId.empty());
  TEST_CHECK(std::abs(opencode->memoryPressure - 4.0) < 1e-9);
  TEST_CHECK(row(service, "pipewire.service")->slice == UnitSlice::Session);
  TEST_CHECK(service.rows().front().key == "opencode.service"); // largest memory first
  TEST_CHECK(opencode->cpuPercent == 0.0); // one sample gives no rate

  // 1 s of CPU in 5 s on 4 CPUs = 5 % of the machine.
  writeCgroup(root, units[2].controlGroup, 1'000'000, 480 * mb, 4.0);
  service.sample(t0 + std::chrono::seconds(5));
  opencode = row(service, "opencode.service");
  TEST_CHECK(std::abs(opencode->cpuPercent - 5.0) < 1e-6);
  TEST_CHECK(std::abs(opencode->ioReadBytesPerSec - 200'000.0) < 1e-6);

  // A stopped unit leaves its row.
  units.erase(units.begin() + 2);
  service.setUnits(units);
  TEST_CHECK(row(service, "opencode.service") == nullptr);

  fs::remove_all(root);
  return 0;
}
