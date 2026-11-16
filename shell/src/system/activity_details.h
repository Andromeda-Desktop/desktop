#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct ActivityRow;
struct SystemdUnit;

// PIDs in a unit's cgroup and its child cgroups; `ids` (optional) gets each cgroup's ID (its cgroup2 inode).
[[nodiscard]] std::vector<int>
cgroupPids(const std::string& cgroupRoot, const std::string& controlGroup, std::vector<std::uint64_t>* ids = nullptr);


// utime + stime of one process in clock ticks (/proc/<pid>/stat), 0 when it has exited.
[[nodiscard]] std::uint64_t readProcessCpuTicks(int pid);
// Times the process's threads were put on a CPU (schedstat field 3, summed over /proc/<pid>/task): the wake-up count.
[[nodiscard]] std::uint64_t readTaskTimeslices(int pid);

// The inspector tab: Overview reads nothing extra; the others read the selected row only.
enum class InspectTab : std::uint8_t { Overview, Network, Files, Threads };

struct SocketDetail {
  std::string local;  // "address:port"
  std::string remote; // empty while listening
  std::uint8_t state = 0; // TCP_ESTABLISHED .. TCP_CLOSING (linux/tcp.h)
  std::uint64_t bytes = 0; // received + sent
  auto operator<=>(const SocketDetail&) const = default;
};

struct ThreadUsage {
  int tid = 0;
  std::string name;
  double cpuPercent = 0.0; // share of all CPUs
};

// What the inspector shows for the selected row beyond the row's own numbers.
struct Inspection {
  InspectTab tab = InspectTab::Overview;
  std::vector<SocketDetail> sockets; // TCP sockets of the row's cgroups
  std::vector<std::string> files;    // open regular files, sorted, at most kMaxInspectFiles
  std::vector<ThreadUsage> threads;  // busiest first, at most kMaxInspectThreads
};
inline constexpr std::size_t kMaxInspectFiles = 500;
inline constexpr std::size_t kMaxInspectThreads = 200;

struct ProcessGroupUsage {
  std::string command; // /proc/<pid>/comm
  int count = 0;
  std::uint64_t residentBytes = 0;
  double cpuPercent = 0.0; // share of all CPUs since the previous update
};

// 0..4 bars from CPU and GPU shares (percent of the whole machine): the Energy column and the "High energy" chip.
[[nodiscard]] int energyBars(double cpuPercent, double gpuPercent);

// Per-row extras that only the open Activity window needs: GPU share, history, process groups of expanded rows.
// update() runs once per ActivityMonitor sample; snapshot() runs from the 30 min background check, window or not.
// The /proc and cgroup reads of a sample run on the SharedWorker; GPU shares and groups apply when they finish.
class ActivityDetails {
public:
  static constexpr std::size_t kShortSamples = 600; // 10 min at the 1 s sample interval
  static constexpr std::size_t kLongSamples = 48;   // 24 h at the 30 min check interval
  static constexpr auto kMinInterval = std::chrono::milliseconds(900);
  static constexpr auto kSnapshotInterval = std::chrono::minutes(25); // a manual check between two must not add a point

  struct RowHistory {
    std::deque<float> memoryMb; // short: one per sample while the window is open
    std::deque<float> cpuPercent;
    std::deque<float> longMemoryMb; // long: one per background check
  };

  // Returns false and changes nothing when called again within kMinInterval (a unit-list refresh, not a sample).
  // `probed` runs on the main loop when the worker's GPU shares and groups for this sample apply.
  bool update(
      const std::vector<ActivityRow>& rows, const std::vector<SystemdUnit>& units, const std::string& cgroupRoot,
      int cpuCount, std::chrono::steady_clock::time_point now, std::function<void()> probed
  );
  void snapshot(const std::vector<ActivityRow>& rows, std::chrono::steady_clock::time_point now);
  // Clears the short history and GPU baselines (window closed: the next open starts fresh).
  void resetShort();

  // What the window shows. Network rates cost a dump of every TCP socket, wake-ups a read per thread: each is only
  // taken while a view shows it.
  struct Shown {
    bool network = false;
    bool wakeups = false;
  };
  void setShown(Shown shown) { m_shown = shown; }
  // The row the inspector shows and its tab; an empty key or Overview reads nothing extra.
  void setInspected(std::string rowKey, InspectTab tab);
  // The last reading for rowKey's current tab, or null before one landed.
  [[nodiscard]] const Inspection* inspection(const std::string& rowKey) const;
  void setExpanded(const std::string& rowKey, bool expanded);
  [[nodiscard]] bool expanded(const std::string& rowKey) const { return m_expanded.contains(rowKey); }

  [[nodiscard]] double gpuPercent(const std::string& rowKey) const;
  // TCP bytes per second (received + sent) since the previous sample; empty before the second sample.
  [[nodiscard]] std::optional<double> networkBytesPerSec(const std::string& rowKey) const;
  // Times per second the row's threads were put on a CPU; empty before the second sample with wake-ups shown.
  [[nodiscard]] std::optional<double> wakeupsPerSec(const std::string& rowKey) const;
  [[nodiscard]] const RowHistory* history(const std::string& rowKey) const;
  [[nodiscard]] const std::vector<ProcessGroupUsage>* groups(const std::string& rowKey) const;
  [[nodiscard]] const std::map<std::string, RowHistory>& histories() const noexcept { return m_history; }

  struct DrmFds {
    std::uint64_t startTime = 0; // /proc/<pid>/stat field 22: a reused pid gets a new scan
    std::vector<int> fds;        // fds that link to /dev/dri/*
    std::chrono::steady_clock::time_point scannedAt;
  };
  // State that only the worker job reads and writes; it moves into the job and back with the result.
  struct ProbeState {
    std::chrono::steady_clock::time_point at; // the previous probe; rates divide by now - at
    // Busy ns by engine per DRM client ("<drm-pdev>/<drm-client-id>"): a client counts from the probe that first saw
    // it, so one found late does not add its whole lifetime to one sample.
    std::map<std::string, std::map<std::string, std::uint64_t>> clientBusyNs;
    std::map<int, std::uint64_t> pidTicks; // CPU ticks of expanded rows' processes at the previous probe
    std::map<int, std::uint64_t> pidSlices; // thread timeslices per process at the previous probe (wake-ups)
    std::map<int, std::uint64_t> tidTicks;  // CPU ticks per thread of the inspected row at the previous probe
    std::map<int, DrmFds> drmFds;
    std::map<std::uint32_t, std::uint64_t> socketBytes; // every dumped TCP socket inode -> bytes at the previous probe
    bool socketsComplete = false; // socketBytes came from a full dump: an inode not in it opened since then
  };
  struct ProbeRow {
    std::string key;
    std::vector<std::string> controlGroups;
    bool expanded = false;
    InspectTab inspect = InspectTab::Overview;
  };
  struct ProbeResult {
    ProbeState state;
    std::map<std::string, double> gpuPercent;
    std::map<std::string, double> networkBytesPerSec;
    std::map<std::string, double> wakeupsPerSec;
    std::string inspectedKey;
    Inspection inspection;
    std::map<std::string, std::vector<ProcessGroupUsage>> groups;
  };
  // The worker job: touches no ActivityDetails member.
  [[nodiscard]] static ProbeResult probe(
      ProbeState state, const std::vector<ProbeRow>& rows, const std::string& cgroupRoot, int cpuCount,
      std::chrono::steady_clock::time_point now, Shown shown
  );

private:
  std::map<std::string, RowHistory> m_history;
  std::map<std::string, double> m_gpuPercent;
  std::map<std::string, double> m_networkBytesPerSec;
  std::map<std::string, double> m_wakeupsPerSec;
  std::set<std::string> m_expanded;
  Shown m_shown;
  std::string m_inspectKey;
  InspectTab m_inspectTab = InspectTab::Overview;
  std::string m_inspectionKey; // the row m_inspection belongs to
  Inspection m_inspection;
  std::map<std::string, std::vector<ProcessGroupUsage>> m_groups;
  ProbeState m_probeState;
  bool m_probing = false;
  std::uint64_t m_generation = 0; // resetShort() bumps it: a job from before the reset is dropped
  std::shared_ptr<int> m_alive = std::make_shared<int>(0);
  std::chrono::steady_clock::time_point m_lastUpdate;
  std::chrono::steady_clock::time_point m_lastSnapshot;
};
