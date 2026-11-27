#include "core/deferred_call.h"
#include "core/fd_watch.h"
#include "core/poll_loop.h"
#include "core/sys/poll.h"
#include "core/sys/sys.h"
#include "core/timer_manager.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <thread>
#include <vector>

// Model test: random sequences of work queued from callbacks through the real PollLoop::run(): deferred calls (also
// from other threads), timers (also cancelled), wakes through an FdWatch that is destroyed in its own callback and
// recreated a deferred call later, and quit from a callback. Each job must run exactly once unless its timer was cancelled, and none after quit.
namespace {
  int g_failures = 0;

  void check(bool ok, const char* what, unsigned seed) {
    if (!ok && ++g_failures <= 20) {
      std::fprintf(stderr, "FAIL seed %u: %s\n", seed, what);
    }
  }

  struct Wake {
    sys::WakeFd fd;
    std::vector<std::function<void()>> waiting;
    std::unique_ptr<FdWatch> watch;
    bool dropWatch = false;

    ~Wake() { watch.reset(); }
    void attach(PollLoop& loop) {
      watch = std::make_unique<FdWatch>(loop, fd.pollFd(), POLLIN, [this, &loop](short revents) {
        if ((revents & POLLIN) == 0) {
          return;
        }
        fd.drain();
        auto jobs = std::move(waiting);
        waiting.clear();
        for (auto& job : jobs) {
          job();
        }
        if (dropWatch) {
          dropWatch = false;
          watch.reset();
          DeferredCall::callLater([this, &loop] { attach(loop); });
        }
      });
    }
    void push(std::function<void()> job) {
      waiting.push_back(std::move(job));
      (void)fd.signal();
    }
  };

  void runSeed(unsigned seed) {
    std::mt19937 rng(seed);
    PollLoop loop;
    Wake wake;
    wake.attach(loop);

    struct Job {
      int runs = 0;
      bool cancelled = false;
      TimerManager::TimerId timer = 0;
    };
    std::vector<Job> jobs;
    jobs.reserve(512);
    int budget = 300;
    int outstanding = 0;
    bool quitCalled = false;
    bool ranAfterQuit = false;
    std::vector<std::thread> threads;

    std::function<void()> spawn;
    auto finish = [&] {
      if (--outstanding == 0) {
        quitCalled = true;
        loop.quit(7);
      }
    };
    auto makeJob = [&]() -> std::function<void()> {
      const std::size_t id = jobs.size();
      jobs.emplace_back();
      ++outstanding;
      return [&, id] {
        if (quitCalled) {
          ranAfterQuit = true;
        }
        ++jobs[id].runs;
        for (int n = static_cast<int>(rng() % 3); n > 0 && budget > 0; --n) {
          spawn();
        }
        finish();
      };
    };
    spawn = [&] {
      --budget;
      switch (rng() % 6) {
      case 0:
        DeferredCall::callLater(makeJob());
        break;
      case 1:
        threads.emplace_back([job = makeJob()]() mutable { DeferredCall::callLater(std::move(job)); });
        break;
      case 2: {
        const std::size_t id = jobs.size();
        auto job = makeJob();
        jobs[id].timer = TimerManager::instance().start(0, std::chrono::milliseconds(rng() % 3), std::move(job));
        break;
      }
      case 3: {
        // Cancel a pending timer; a cancelled job counts as finished.
        for (std::size_t id = 0; id < jobs.size(); ++id) {
          Job& job = jobs[id];
          if (job.timer != 0 && job.runs == 0 && !job.cancelled && TimerManager::instance().cancel(job.timer)) {
            job.cancelled = true;
            finish();
            break;
          }
        }
        break;
      }
      case 4:
        wake.push(makeJob());
        break;
      default:
        wake.dropWatch = true;
        wake.push(makeJob());
        break;
      }
    };

    DeferredCall::callLater([&] {
      ++outstanding;
      for (int n = 0; n < 4; ++n) {
        spawn();
      }
      finish();
    });
    const int code = loop.run();
    for (auto& thread : threads) {
      thread.join();
    }
    check(code == 7, "run() did not return the quit code", seed);
    check(!ranAfterQuit, "a job ran after quit", seed);
    for (const Job& job : jobs) {
      check(job.runs == (job.cancelled ? 0 : 1), "a job did not run exactly once (or ran after cancel)", seed);
    }
    // Leftovers from this seed must not leak into the next.
    while (!DeferredCall::takePending().empty()) {
    }
  }
} // namespace

int main() {
  for (unsigned seed = 1; seed <= 200; ++seed) {
    runSeed(seed);
  }
  PollLoop quitting;
  quitting.quit(3);
  check(!quitting.runDeferred() && quitting.run() == 3, "a quit loop ran another pass", 0);

  // A worker loop runs only its own sources, and a quit stops the rest of its pass.
  {
    const sys::WakeFd a;
    const sys::WakeFd b;
    check(a.valid() && b.valid() && a.signal() && b.signal(), "wake setup", 0);
    bool deferredRan = false;
    bool timerRan = false;
    bool secondRan = false;
    DeferredCall::callLater([&] { deferredRan = true; });
    Timer timer;
    timer.start(std::chrono::milliseconds(0), [&] { timerRan = true; });
    PollLoop worker(PollLoop::Kind::Worker);
    const FdWatch first(worker, a.pollFd(), POLLIN, [&](short) { worker.quit(5); });
    const FdWatch second(worker, b.pollFd(), POLLIN, [&](short) { secondRan = true; });
    check(worker.run() == 5, "worker loop did not return its quit code", 0);
    check(!deferredRan && !timerRan, "a worker loop ran main-thread deferred calls or timers", 0);
    check(!secondRan, "a source ran after quit in the same pass", 0);
    PollLoop main;
    DeferredCall::callLater([&] { main.quit(); });
    main.run();
    check(deferredRan, "the main loop did not run the deferred call", 0);
  }
  // As poll(2) on every OS: a closed entry reports POLLNVAL and a signaled one beside it still reports POLLIN.
  {
    const sys::WakeFd live;
    int closed = -1;
    {
      const sys::WakeFd gone;
      closed = gone.pollFd();
    }
    check(live.valid() && live.signal(), "wake setup", 0);
    pollfd fds[2] = {{closed, POLLIN, 0}, {live.pollFd(), POLLIN, 0}};
    const int ready = sys::pollFds(fds, 2, 0);
    check(ready == 2 && fds[0].revents == POLLNVAL && fds[1].revents == POLLIN,
          "a closed entry failed the wait or hid a signaled one", 0);
  }
  std::printf("poll_loop: %s\n", g_failures == 0 ? "ok" : "FAILED");
  return g_failures == 0 ? 0 : 1;
}
