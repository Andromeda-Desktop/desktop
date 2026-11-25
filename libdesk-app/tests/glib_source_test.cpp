#include "app/glib_source.h"
#include "core/poll_loop.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
#include <unistd.h>
#include <vector>

// Other threads attach GLib sources at random times (as GDBus does for signal callbacks) while a PollLoop with only a
// GlibSource sleeps in poll(): every attached source runs. A lost wakeup hangs the loop; alarm() then fails the test.
namespace {

struct Round {
  PollLoop* loop;
  int expected;
  std::atomic<int> ran{0};
};

gboolean onIdle(gpointer data) {
  auto* round = static_cast<Round*>(data);
  if (round->ran.fetch_add(1) + 1 == round->expected) {
    round->loop->quit(0);
  }
  return G_SOURCE_REMOVE;
}

} // namespace

int main() {
  alarm(20);
  constexpr int kThreads = 4;
  constexpr int kPerThread = 50;
  for (unsigned seed = 1; seed <= 20; ++seed) {
    PollLoop loop;
    GlibSource glib;
    loop.addSource(&glib);
    Round round{.loop = &loop, .expected = kThreads * kPerThread};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&round, seed, t] {
        std::mt19937 rng(seed * 31 + static_cast<unsigned>(t));
        for (int i = 0; i < kPerThread; ++i) {
          std::this_thread::sleep_for(std::chrono::microseconds(rng() % 2000));
          g_idle_add(onIdle, &round);
        }
      });
    }
    const int code = loop.run();
    for (std::thread& thread : threads) {
      thread.join();
    }
    loop.removeSource(&glib);
    if (code != 0 || round.ran.load() != round.expected) {
      std::fprintf(stderr, "FAIL seed %u: ran %d of %d, code %d\n", seed, round.ran.load(), round.expected, code);
      return 1;
    }
  }
  return 0;
}
