#pragma once

#include <atomic>

// Exiting starts when the app loop has ended: teardown may then leave memory to the OS instead of freeing it.
enum class ProcessPhase : unsigned char { Running, Exiting };

namespace process_phase_detail {
  inline std::atomic<ProcessPhase> g_phase{ProcessPhase::Running};
}

inline void beginProcessExit() noexcept { process_phase_detail::g_phase.store(ProcessPhase::Exiting); }
[[nodiscard]] inline bool processExiting() noexcept {
  return process_phase_detail::g_phase.load(std::memory_order_relaxed) == ProcessPhase::Exiting;
}
