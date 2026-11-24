#include "core/blocking_pool.h"

#include <utility>

BlockingPool& BlockingPool::instance() {
  // Never destroyed: a job may still run while static destructors run at exit.
  static auto* pool = new BlockingPool;
  return *pool;
}

BlockingPool::BlockingPool() : m_slots(kMaxThreads, "blocking") {}

void BlockingPool::run(std::function<void()> work) {
  const std::lock_guard lock(m_mutex);
  m_queue.push_back(std::move(work));
  if (m_queue.size() <= m_idle) {
    m_wake.notify_one();
  } else if (m_threads < kMaxThreads) {
    ++m_threads;
    m_slots.spawn([this](std::size_t slot) { loop(slot); });
  }
}

std::size_t BlockingPool::threadCount() {
  const std::lock_guard lock(m_mutex);
  return m_threads;
}

void BlockingPool::loop(std::size_t slot) {
  std::unique_lock lock(m_mutex);
  while (true) {
    if (m_queue.empty()) {
      ++m_idle;
      const bool woke =
          m_wake.wait_for(lock, IdleWorkerSlots::kIdleExit, [this] { return !m_queue.empty(); });
      --m_idle;
      if (!woke) {
        --m_threads;
        m_slots.exited(slot);
        return;
      }
    }
    auto job = std::move(m_queue.front());
    m_queue.pop_front();
    lock.unlock();
    job();
    lock.lock();
  }
}
