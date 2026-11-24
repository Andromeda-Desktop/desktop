// Model test for BlockingPool: random posts from owners that die at random points, with completions delivered at
// random times. Invariants: never more than kMaxThreads threads or running jobs; every job runs once; no completion
// runs after its owner died; a full pool queues extra work instead of starting threads.

#include "core/blocking_pool.h"
#include "core/deferred_call.h"
#include "tests/test_deferred.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <print>
#include <random>
#include <thread>
#include <vector>

namespace {
  int gFailures = 0;

  void expect(bool condition, const char* message, unsigned seed) {
    if (!condition) {
      std::println(stderr, "blocking_pool_test: seed {}: {}", seed, message);
      ++gFailures;
    }
  }

  std::atomic<int> gRunning{0};
  std::atomic<int> gMaxRunning{0};

  void runSeed(unsigned seed) {
    std::mt19937 rng(seed);
    struct Owner {
      std::shared_ptr<int> alive = std::make_shared<int>(0);
      bool dead = false;
    };
    std::vector<std::shared_ptr<Owner>> owners;
    auto worked = std::make_shared<std::atomic<int>>(0);
    int posted = 0;
    // Heap counters: a completion posted after this seed ends still has somewhere to write.
    struct Deliveries {
      int count = 0;
      int late = 0;
    };
    auto deliveries = std::make_shared<Deliveries>();
    for (int op = 0; op < 300; ++op) {
      switch (rng() % 6) {
      case 0:
        owners.push_back(std::make_shared<Owner>());
        break;
      case 1:
        if (!owners.empty()) {
          auto& owner = owners[rng() % owners.size()];
          owner->alive.reset();
          owner->dead = true;
        }
        break;
      case 2:
        for (auto& call : DeferredCall::takePending()) {
          call();
        }
        break;
      default:
        if (!owners.empty()) {
          auto owner = owners[rng() % owners.size()];
          if (owner->dead) {
            break;
          }
          const auto sleep = std::chrono::microseconds(rng() % 1500);
          ++posted;
          BlockingPool::instance().run<int>(
              owner->alive,
              [worked, sleep] {
                const int now = ++gRunning;
                int seen = gMaxRunning.load();
                while (now > seen && !gMaxRunning.compare_exchange_weak(seen, now)) {
                }
                std::this_thread::sleep_for(sleep);
                --gRunning;
                ++*worked;
                return 1;
              },
              [owner, deliveries](int) {
                ++deliveries->count;
                deliveries->late += owner->dead ? 1 : 0;
              }
          );
        }
        break;
      }
      expect(BlockingPool::instance().threadCount() <= BlockingPool::kMaxThreads, "thread count over the cap", seed);
    }
    expect(runDeferredUntil([&] { return worked->load() == posted; }, std::chrono::seconds(20)), "a job never ran",
           seed);
    for (auto& call : DeferredCall::takePending()) {
      call();
    }
    expect(deliveries->late == 0, "a completion ran after its owner died", seed);
    expect(deliveries->count <= posted, "a completion ran twice", seed);
    expect(gMaxRunning.load() <= static_cast<int>(BlockingPool::kMaxThreads), "more jobs ran at once than the cap",
           seed);
  }

  void fullPoolQueues() {
    std::mutex mutex;
    std::condition_variable released;
    bool open = false;
    std::atomic<int> started{0};
    std::atomic<int> finished{0};
    constexpr int kJobs = static_cast<int>(BlockingPool::kMaxThreads) + 4;
    for (int i = 0; i < kJobs; ++i) {
      BlockingPool::instance().run([&] {
        ++started;
        std::unique_lock lock(mutex);
        released.wait(lock, [&] { return open; });
        ++finished;
      });
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (started.load() < static_cast<int>(BlockingPool::kMaxThreads) && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    expect(started.load() == static_cast<int>(BlockingPool::kMaxThreads), "a full pool did not queue extra jobs", 0);
    expect(BlockingPool::instance().threadCount() == BlockingPool::kMaxThreads, "a full pool has the wrong size", 0);
    {
      const std::lock_guard lock(mutex);
      open = true;
    }
    released.notify_all();
    while (finished.load() < kJobs && std::chrono::steady_clock::now() < deadline + std::chrono::seconds(5)) {
      std::this_thread::yield();
    }
    expect(finished.load() == kJobs, "queued jobs did not run after the pool freed up", 0);
  }
} // namespace

int main() {
  for (unsigned seed = 1; seed <= 8; ++seed) {
    runSeed(seed);
  }
  fullPoolQueues();
  return gFailures == 0 ? 0 : 1;
}
