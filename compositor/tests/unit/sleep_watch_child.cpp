// SleepWatch on the system bus that DBUS_SYSTEM_BUS_ADDRESS names, driven by tests/sleep_watch_bus.py: prints
// "sleeping 0|1" and "late" for its callbacks, and calls ready() for each "ready" line on stdin.
#include "server/sleep_watch.h"

#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <wayland-server-core.h>

namespace {
  bool running = true;

  int onStdin(int fd, uint32_t /*mask*/, void* data) {
    char buffer[64];
    const ssize_t n = read(fd, buffer, sizeof(buffer));
    if (n <= 0) {
      running = false;
      return 0;
    }
    if (memmem(buffer, static_cast<size_t>(n), "ready", 5) != nullptr) {
      static_cast<andromeda::SleepWatch*>(data)->ready();
    }
    return 0;
  }
} // namespace

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  wl_event_loop* loop = wl_event_loop_create();
  {
    andromeda::SleepWatch watch(
        loop, [](bool sleeping) { std::printf("sleeping %d\n", sleeping ? 1 : 0); }, [] { std::printf("late\n"); }
    );
    wl_event_loop_add_fd(loop, STDIN_FILENO, WL_EVENT_READABLE, onStdin, &watch);
    while (running) {
      wl_event_loop_dispatch(loop, -1);
    }
  }
  wl_event_loop_destroy(loop);
  return 0;
}
