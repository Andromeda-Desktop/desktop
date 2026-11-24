#pragma once

#include "core/deferred_call.h"
#include "core/idle_worker_slots.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

// The process's one pool for blocking work we start (file and subprocess I/O, PAM, network waits): at most
// kMaxThreads threads, started with queued work, exiting after IdleWorkerSlots::kIdleExit idle. Work must not touch
// its owner; it returns data, and the completion runs on the main loop only while the owner's alive token lives.
class BlockingPool {
public:
  static constexpr std::size_t kMaxThreads = 16;

  static BlockingPool& instance();

  BlockingPool(const BlockingPool&) = delete;
  BlockingPool& operator=(const BlockingPool&) = delete;

  void run(std::function<void()> work);

  template <typename Result>
  void run(std::weak_ptr<const void> alive, std::function<Result()> work, std::function<void(Result)> done) {
    run([alive = std::move(alive), work = std::move(work), done = std::move(done)]() mutable {
      auto result = std::make_shared<Result>(work());
      DeferredCall::callLaterIfAlive(alive, [done = std::move(done), result] { done(std::move(*result)); });
    });
  }

  // Threads alive now; for the thread budget check.
  [[nodiscard]] std::size_t threadCount();

private:
  BlockingPool();
  void loop(std::size_t slot);

  std::mutex m_mutex;
  std::condition_variable m_wake;
  std::deque<std::function<void()>> m_queue;
  std::size_t m_threads = 0;
  std::size_t m_idle = 0;
  IdleWorkerSlots m_slots;
};
