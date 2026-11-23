// Model test for bus::Client: random create, destroy, call and bus-loss sequences against a private dbus-daemon.
// Invariants: clients of one loop share one connection, another loop gets its own; a reply never reaches a destroyed
// client; every call of a live client gets exactly one reply; after a loss every client sees nullptr, then the new bus.
#include "core/loop_bus.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <systemd/sd-bus.h>
#include <unistd.h>
#include <vector>
#include <wayland-server-core.h>

namespace {

  using andromeda::bus::Client;
  using andromeda::bus::Kind;

  int g_failures = 0;

  void check(bool ok, const std::string& what) {
    if (!ok) {
      std::fprintf(stderr, "FAIL: %s\n", what.c_str());
      ++g_failures;
    }
  }

  struct Daemon {
    std::string address;
    pid_t pid = -1;

    void start() {
      int fds[2];
      if (pipe(fds) != 0) {
        std::perror("pipe");
        std::exit(2);
      }
      pid = fork();
      if (pid == 0) {
        close(fds[0]);
        const std::string printFd = "--print-address=" + std::to_string(fds[1]);
        const std::string addr = "--address=" + address;
        execlp(
            "dbus-daemon", "dbus-daemon", "--session", "--nofork", "--nopidfile", addr.c_str(), printFd.c_str(),
            static_cast<char*>(nullptr)
        );
        _exit(127);
      }
      close(fds[1]);
      char buffer[512];
      const ssize_t n = read(fds[0], buffer, sizeof buffer);
      close(fds[0]);
      if (n <= 0) {
        std::fprintf(stderr, "dbus-daemon did not start\n");
        std::exit(77);
      }
    }

    void stop() {
      if (pid > 0) {
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        pid = -1;
      }
    }
  };

  struct Model {
    wl_event_loop* loop = nullptr;
    struct Entry {
      std::unique_ptr<Client> client;
      std::vector<sd_bus*> changes;
    };
    std::map<int, Entry> clients;
    std::set<int> dead;
    // call id -> client id, for calls not answered yet
    std::map<int, int> pending;
    std::map<int, int> answers;
    int nextClient = 0;
    int nextCall = 0;

    int create() {
      const int id = nextClient++;
      auto& entry = clients[id];
      entry.client = std::make_unique<Client>(loop, Kind::User);
      entry.client->onChange([this, id](sd_bus* bus) { clients[id].changes.push_back(bus); });
      return id;
    }

    void destroy(int id) {
      clients.erase(id);
      dead.insert(id);
      std::erase_if(pending, [id](const auto& call) { return call.second == id; });
    }

    void call(int id) {
      Client& client = *clients.at(id).client;
      sd_bus_message* message = nullptr;
      if (client.get() == nullptr
          || sd_bus_message_new_method_call(
                 client.get(), &message, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                 "GetId"
             ) < 0) {
        return;
      }
      const int callId = nextCall++;
      const bool sent = client.call(message, [this, id, callId](sd_bus_message* /*reply*/) {
        check(!dead.contains(id), "reply reached destroyed client " + std::to_string(id));
        ++answers[callId];
        check(answers[callId] == 1, "call " + std::to_string(callId) + " answered twice");
        pending.erase(callId);
      });
      sd_bus_message_unref(message);
      if (sent) {
        pending[callId] = id;
      }
    }

    void pump(int ms) {
      const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
      while (std::chrono::steady_clock::now() < until) {
        wl_event_loop_dispatch(loop, 10);
      }
    }

    bool settle(int maxMs) {
      const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMs);
      while (!pending.empty() && std::chrono::steady_clock::now() < until) {
        wl_event_loop_dispatch(loop, 10);
      }
      return pending.empty();
    }

    void checkShared(const std::string& when) {
      sd_bus* first = nullptr;
      for (const auto& [id, entry] : clients) {
        sd_bus* bus = entry.client->get();
        check(bus != nullptr, when + ": client " + std::to_string(id) + " has no bus");
        if (first == nullptr) {
          first = bus;
        }
        check(bus == first, when + ": client " + std::to_string(id) + " has its own connection");
      }
    }
  };

} // namespace

int main() {
  const auto dir = std::filesystem::temp_directory_path() / ("loop-bus-" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  Daemon daemon{.address = "unix:path=" + (dir / "bus").string()};
  daemon.start();
  setenv("DBUS_SESSION_BUS_ADDRESS", daemon.address.c_str(), 1);

  constexpr int kSeeds = 12;
  for (int seed = 0; seed < kSeeds; ++seed) {
    std::mt19937 random(static_cast<unsigned>(seed));
    Model model;
    model.loop = wl_event_loop_create();
    wl_event_loop* other = wl_event_loop_create();
    // Seeds 0, 4 and 8 lose the bus once mid-run.
    const int loseAt = seed % 4 == 0 ? 60 : -1;
    for (int step = 0; step < 120; ++step) {
      const std::string when = "seed " + std::to_string(seed) + " step " + std::to_string(step);
      if (step == loseAt && !model.clients.empty()) {
        for (auto& [id, entry] : model.clients) {
          entry.changes.clear();
        }
        daemon.stop();
        std::filesystem::remove(dir / "bus");
        daemon.start();
        // The owner sees the hangup, answers pending calls with nullptr, and opens again after 2 s.
        model.pump(2600);
        check(model.pending.empty(), when + ": calls pending across the loss");
        for (const auto& [id, entry] : model.clients) {
          check(
              entry.changes.size() == 2 && entry.changes[0] == nullptr && entry.changes[1] != nullptr,
              when + ": client " + std::to_string(id) + " did not see loss then reopen"
          );
        }
        model.checkShared(when + " after reopen");
        continue;
      }
      const int op = static_cast<int>(random() % 10);
      if (op < 3 || model.clients.empty()) {
        model.create();
      } else if (op < 5) {
        auto it = model.clients.begin();
        std::advance(it, static_cast<long>(random() % model.clients.size()));
        model.destroy(it->first);
      } else if (op < 9) {
        auto it = model.clients.begin();
        std::advance(it, static_cast<long>(random() % model.clients.size()));
        model.call(it->first);
      } else {
        wl_event_loop_dispatch(model.loop, 0);
      }
      model.checkShared(when);
    }
    check(model.settle(3000), "seed " + std::to_string(seed) + ": calls of live clients never answered");
    {
      Client a(model.loop, Kind::User);
      Client b(other, Kind::User);
      check(
          a.get() != nullptr && b.get() != nullptr && a.get() != b.get(),
          "seed " + std::to_string(seed) + ": two loops share a connection"
      );
    }
    model.clients.clear();
    wl_event_loop_destroy(other);
    wl_event_loop_destroy(model.loop);
  }
  daemon.stop();
  std::filesystem::remove_all(dir);
  if (g_failures != 0) {
    std::fprintf(stderr, "%d failures\n", g_failures);
    return 1;
  }
  std::printf("loop-bus: %d seeds ok\n", kSeeds);
  return 0;
}
