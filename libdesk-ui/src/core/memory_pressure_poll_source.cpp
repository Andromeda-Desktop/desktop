#include "core/memory_pressure_poll_source.h"

#include "core/log.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace {
  constexpr Logger kLog("memory");
  // 150 ms of "some" stall in a 2 s window; unprivileged triggers need a window that is a multiple of 2 s.
  constexpr char kTrigger[] = "some 150000 2000000"; // kStallUs, kWindowUs
  constexpr std::uint64_t kStallUs = 150000;
  constexpr std::uint64_t kWindowUs = 2000000;
  constexpr auto kMinInterval = std::chrono::minutes(1);
  // Kernel 7.3 fires unprivileged triggers on /proc/pressure and parent cgroups with no stall growth (71 us of 150 ms
  // measured): an event counts only when the "some" total grew at the trigger's rate since the last event or start.
  constexpr auto kConfirmSpan = std::chrono::seconds(10);

  std::optional<std::uint64_t> someTotalUs(const std::string& path) {
    std::ifstream file(path);
    std::string line;
    if (!std::getline(file, line) || !line.starts_with("some ")) {
      return std::nullopt;
    }
    const auto at = line.find("total=");
    return at == std::string::npos ? std::nullopt : std::optional(std::stoull(line.substr(at + 6)));
  }
} // namespace

std::string MemoryPressurePollSource::ownCgroupPressureFile() {
  std::ifstream cgroup("/proc/self/cgroup");
  std::string line;
  if (!std::getline(cgroup, line) || !line.starts_with("0::")) {
    return {};
  }
  return "/sys/fs/cgroup" + line.substr(3) + "/memory.pressure";
}

MemoryPressurePollSource::MemoryPressurePollSource(const std::string& pressureFile, std::function<void()> onPressure)
    : m_path(pressureFile), m_onPressure(std::move(onPressure)) {
  if (pressureFile.empty()) {
    return;
  }
  m_fd = open(pressureFile.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (m_fd >= 0 && write(m_fd, kTrigger, sizeof(kTrigger)) < 0) {
    close(m_fd);
    m_fd = -1;
  }
  if (m_fd < 0) {
    kLog.info("no memory pressure trigger on {}: {}", pressureFile, std::strerror(errno));
    return;
  }
  m_sample = {std::chrono::steady_clock::now(), someTotalUs(m_path)};
}

MemoryPressurePollSource::~MemoryPressurePollSource() {
  if (m_fd >= 0) {
    close(m_fd);
  }
}

void MemoryPressurePollSource::doAddPollFds(std::vector<pollfd>& fds) {
  if (m_fd >= 0) {
    fds.push_back({.fd = m_fd, .events = POLLPRI, .revents = 0});
  }
}

void MemoryPressurePollSource::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (m_fd < 0 || startIdx >= fds.size()) {
    return;
  }
  const short revents = fds[startIdx].revents;
  if ((revents & POLLERR) != 0) {
    // The cgroup went away: no more events can come.
    close(m_fd);
    m_fd = -1;
    return;
  }
  if ((revents & POLLPRI) == 0) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  const Sample sample{now, someTotalUs(m_path)};
  const Sample last = std::exchange(m_sample, sample);
  const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(now - last.at).count();
  const bool confirmed = sample.totalUs && last.totalUs && now - last.at <= kConfirmSpan
      && (*sample.totalUs - *last.totalUs) * kWindowUs >= kStallUs * static_cast<std::uint64_t>(elapsedUs);
  if (confirmed && now - m_last >= kMinInterval) {
    m_last = now;
    kLog.info("memory pressure");
    m_onPressure();
  }
}
