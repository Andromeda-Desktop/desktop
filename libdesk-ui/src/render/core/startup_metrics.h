#pragma once

#include <string_view>

// Startup breakdown for the launch-time budgets (docs/performance.md). Main thread only.
namespace startup_metrics {

  // Milliseconds since the process started (kernel start time, 10 ms resolution).
  [[nodiscard]] double sinceProcessStart();
  void recordEglInit(double ms);
  // Time the main thread blocked waiting for the EGL worker.
  void recordEglWait(double ms);
  void recordProgramBuild(double ms);
  void recordProgramPreloaded();
  // Logs the breakdown once, on the first presented frame.
  // True on the first call only.
  bool recordFirstFrame();
  // With ANDROMEDA_PROFILE set, logs when one named surface first presents.
  void recordSurfaceFirstFrame(std::string_view surface);

} // namespace startup_metrics
