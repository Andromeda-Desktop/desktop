#pragma once

#include <algorithm>
#include <chrono>
#include <random>

// Retry delays for reconnects and failed work (C12): doubling from first to cap, each spread by jitter so clients
// that failed together do not retry together. The standard library only, so every binary can compile it; the Rust
// mirror is andromeda_rt::backoff.
namespace retry {

  class Backoff {
  public:
    using Duration = std::chrono::milliseconds;

    constexpr Backoff(Duration first, Duration cap) : m_first(first), m_cap(cap), m_step(first) {}

    // The wait before the next try: a random point in [step/2, step]. Each call doubles step up to cap.
    [[nodiscard]] Duration nextDelay() {
      const Duration step = m_step;
      m_step = std::min(step * 2, m_cap);
      thread_local std::minstd_rand rng{std::random_device{}()};
      return step - Duration(std::uniform_int_distribution<Duration::rep>(0, step.count() / 2)(rng));
    }

    // After a success: the next failure waits about first again.
    void reset() { m_step = m_first; }

  private:
    Duration m_first;
    Duration m_cap;
    Duration m_step;
  };

} // namespace retry
