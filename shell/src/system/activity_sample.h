#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// One thread at one moment, from /proc/<pid>/task/<tid>/{stat,wchan}.
struct ThreadSnapshot {
  int pid = 0;
  int tid = 0;
  std::string name;
  char state = '?';        // stat field 3: R running, S sleeping, D uninterruptible wait, ...
  std::uint64_t ticks = 0; // utime + stime, clock ticks
  std::string wchan;       // kernel function the thread waits in; empty while it runs or when hidden
};

// Every thread of `pids`; threads that exit while it reads are skipped. Blocking file reads: call off the main thread.
[[nodiscard]] std::vector<ThreadSnapshot> snapshotThreads(const std::vector<int>& pids);

// Parses one task stat line into `out` (name, state, ticks); false for a malformed line.
bool parseTaskStat(std::string_view stat, ThreadSnapshot& out);

// One thread in a sample result. CPU % is of one core.
struct SampleThread {
  std::string name;
  int tid = 0;
  char state = '?';
  double percent = 0.0;
  std::string wait; // where it waits; "–" while running, "?" when the kernel hides it
};

struct SampleSummary {
  std::string title;
  double seconds = 0.0;
  std::time_t when = 0;
  std::size_t processes = 0;
  std::vector<SampleThread> threads; // every thread, busiest first
  double totalPercent = 0.0;
  std::vector<std::pair<std::string, int>> waits; // wait place -> threads there, most first
};

// A thread missing from `before` started during the sample and counts from 0.
[[nodiscard]] SampleSummary summarizeSample(
    std::string title, const std::vector<ThreadSnapshot>& before, const std::vector<ThreadSnapshot>& after,
    double seconds, long ticksPerSecond, std::time_t when
);
// Plain-text report for the clipboard: the busiest threads, then where the threads wait.
[[nodiscard]] std::string sampleReport(const SampleSummary& summary);
