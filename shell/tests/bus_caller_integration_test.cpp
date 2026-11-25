#include "dbus/caller.h"
#include "tests/test_check.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <sdbus-c++/sdbus-c++.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

using namespace std::chrono_literals;

// busCaller tells the host from a Flatpak app: a bwrap child with a /.flatpak-info is the sandboxed app it names.
int main() {
  auto bus = sdbus::createSessionBusConnection();
  const auto self = busCaller(*bus, bus->getUniqueName());
  TEST_CHECK(self.has_value() && self->kind == BusCaller::Kind::Host);

  const std::string info = "/tmp/bus-caller-test-" + std::to_string(::getpid()) + ".flatpak-info";
  std::ofstream(info) << "[Context]\nshared=network\n\n[Application]\nname=org.test.Sandboxed\nruntime=x\n";
  const char* script = "from gi.repository import Gio, GLib\n"
                       "bus = Gio.bus_get_sync(Gio.BusType.SESSION)\n"
                       "bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',"
                       " 'RequestName', GLib.Variant('(su)', ('org.test.Sandboxed', 4)), None, 0, 5000, None)\n"
                       "GLib.MainLoop().run()\n";
  const char* argv[] = {"bwrap", "--ro-bind", "/usr", "/usr", "--symlink", "usr/bin", "/bin", "--symlink", "usr/lib",
                        "/lib", "--symlink", "usr/lib", "/lib64", "--proc", "/proc", "--dev", "/dev", "--bind", "/tmp",
                        "/tmp", "--ro-bind", info.c_str(), "/.flatpak-info", "--", "python3", "-I", "-c", script,
                        nullptr};
  pid_t child = 0;
  TEST_CHECK(::posix_spawnp(&child, "bwrap", nullptr, nullptr, const_cast<char**>(argv), environ) == 0);

  auto daemon = sdbus::createProxy(*bus, sdbus::ServiceName{"org.freedesktop.DBus"},
                                   sdbus::ObjectPath{"/org/freedesktop/DBus"});
  std::string owner;
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (owner.empty() && std::chrono::steady_clock::now() < deadline) {
    try {
      daemon->callMethod("GetNameOwner")
          .onInterface("org.freedesktop.DBus")
          .withArguments(std::string{"org.test.Sandboxed"})
          .storeResultsTo(owner);
    } catch (const sdbus::Error&) {
      std::this_thread::sleep_for(20ms);
    }
  }
  TEST_CHECK(!owner.empty());
  const auto app = busCaller(*bus, owner);
  TEST_CHECK(app.has_value() && app->kind == BusCaller::Kind::Sandboxed && app->appId == "org.test.Sandboxed");

  ::kill(child, SIGTERM);
  ::waitpid(child, nullptr, 0);
  std::remove(info.c_str());
  return 0;
}
