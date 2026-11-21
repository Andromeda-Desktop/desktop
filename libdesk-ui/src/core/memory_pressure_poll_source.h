#pragma once

#include "core/poll_source.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

// Runs onPressure when tasks stall on memory (a PSI trigger on pressureFile, docs/qos.md §5), at most once a minute.
// No trigger, and no calls, where the file is missing (no PSI, macOS).
class MemoryPressurePollSource final : public PollSource {
public:
  static constexpr const char* kSystemPressureFile = "/proc/pressure/memory";
  // memory.pressure of this process's cgroup; empty without cgroup v2.
  [[nodiscard]] static std::string ownCgroupPressureFile();

  MemoryPressurePollSource(const std::string& pressureFile, std::function<void()> onPressure);
  ~MemoryPressurePollSource() override;
  MemoryPressurePollSource(const MemoryPressurePollSource&) = delete;
  MemoryPressurePollSource& operator=(const MemoryPressurePollSource&) = delete;

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  struct Sample {
    std::chrono::steady_clock::time_point at;
    std::optional<std::uint64_t> totalUs; // the "some" stall total
  };
  int m_fd = -1;
  std::string m_path;
  Sample m_sample;
  std::function<void()> m_onPressure;
  std::chrono::steady_clock::time_point m_last{};
};
