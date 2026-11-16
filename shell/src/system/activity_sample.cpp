#include "system/activity_sample.h"

#include <algorithm>
#include <charconv>
#include <dirent.h>
#include <fcntl.h>
#include <format>
#include <map>
#include <unistd.h>
#include <unordered_map>

namespace {

  constexpr std::size_t kReportThreads = 30;
  constexpr std::size_t kReportWaits = 12;

  std::string readSmall(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      return {};
    }
    char buffer[1024];
    const ssize_t n = ::read(fd, buffer, sizeof(buffer));
    ::close(fd);
    return n > 0 ? std::string(buffer, static_cast<std::size_t>(n)) : std::string();
  }

  std::vector<int> numericEntries(const std::string& dir) {
    std::vector<int> ids;
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) {
      return ids;
    }
    while (const dirent* entry = ::readdir(d)) {
      int id = 0;
      const std::string_view name = entry->d_name;
      if (std::from_chars(name.data(), name.data() + name.size(), id).ec == std::errc{} && id > 0) {
        ids.push_back(id);
      }
    }
    ::closedir(d);
    return ids;
  }

} // namespace

bool parseTaskStat(std::string_view stat, ThreadSnapshot& out) {
  // comm may hold spaces and ')': it runs from the first '(' to the last ')'. utime and stime are fields 14 and 15.
  const std::size_t open = stat.find('(');
  const std::size_t close = stat.rfind(')');
  if (open == std::string_view::npos || close == std::string_view::npos || close < open || close + 2 >= stat.size()) {
    return false;
  }
  out.name = std::string(stat.substr(open + 1, close - open - 1));
  std::string_view rest = stat.substr(close + 2);
  out.state = rest.front();
  out.ticks = 0;
  for (int field = 3; field <= 15; ++field) {
    const std::size_t space = rest.find(' ');
    if (field >= 14) {
      std::uint64_t value = 0;
      const std::string_view text = rest.substr(0, space);
      if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{}) {
        return false;
      }
      out.ticks += value;
    }
    if (space == std::string_view::npos) {
      return field == 15;
    }
    rest.remove_prefix(space + 1);
  }
  return true;
}

std::vector<ThreadSnapshot> snapshotThreads(const std::vector<int>& pids) {
  std::vector<ThreadSnapshot> threads;
  for (const int pid : pids) {
    const std::string task = std::format("/proc/{}/task", pid);
    for (const int tid : numericEntries(task)) {
      const std::string base = std::format("{}/{}/", task, tid);
      ThreadSnapshot thread{.pid = pid, .tid = tid};
      if (!parseTaskStat(readSmall(base + "stat"), thread)) {
        continue;
      }
      thread.wchan = readSmall(base + "wchan");
      if (thread.wchan == "0") {
        thread.wchan.clear();
      }
      threads.push_back(std::move(thread));
    }
  }
  return threads;
}

SampleSummary summarizeSample(
    std::string title, const std::vector<ThreadSnapshot>& before, const std::vector<ThreadSnapshot>& after,
    double seconds, long ticksPerSecond, std::time_t when
) {
  std::unordered_map<int, std::uint64_t> start;
  for (const auto& thread : before) {
    start.emplace(thread.tid, thread.ticks);
  }
  SampleSummary summary{.title = std::move(title), .seconds = seconds, .when = when};
  std::vector<int> processes;
  std::map<std::string, int> waits;
  for (const auto& thread : after) {
    const auto it = start.find(thread.tid);
    const std::uint64_t from = it != start.end() && it->second <= thread.ticks ? it->second : 0;
    const double percent = seconds > 0.0 && ticksPerSecond > 0
        ? 100.0 * static_cast<double>(thread.ticks - from) / (static_cast<double>(ticksPerSecond) * seconds)
        : 0.0;
    std::string wait = thread.state == 'R' ? std::string("–") : thread.wchan.empty() ? std::string("?") : thread.wchan;
    ++waits[wait];
    summary.threads.push_back({.name = thread.name, .tid = thread.tid, .state = thread.state, .percent = percent,
                               .wait = std::move(wait)});
    summary.totalPercent += percent;
    processes.push_back(thread.pid);
  }
  std::ranges::sort(processes);
  const auto unique = std::ranges::unique(processes);
  summary.processes = static_cast<std::size_t>(std::distance(processes.begin(), unique.begin()));
  std::ranges::stable_sort(summary.threads, [](const auto& a, const auto& b) { return a.percent > b.percent; });
  summary.waits.assign(waits.begin(), waits.end());
  std::ranges::stable_sort(summary.waits, [](const auto& a, const auto& b) { return a.second > b.second; });
  return summary;
}

std::string sampleReport(const SampleSummary& summary) {
  std::tm local{};
  localtime_r(&summary.when, &local);
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
  std::string report = std::format(
      "Sample of {}: {:.0f} s from {}\n{} processes, {} threads, {:.1f} % CPU (100 % = one core)\n\n", summary.title,
      summary.seconds, stamp, summary.processes, summary.threads.size(), summary.totalPercent
  );
  report += std::format("{:>6}  {:<5}  {:<16}  {:>8}  {}\n", "CPU %", "State", "Thread", "TID", "Waiting in");
  for (std::size_t i = 0; i < summary.threads.size() && i < kReportThreads; ++i) {
    const SampleThread& t = summary.threads[i];
    report += std::format("{:>6.1f}  {:<5}  {:<16}  {:>8}  {}\n", t.percent, t.state, t.name, t.tid, t.wait);
  }
  if (summary.threads.size() > kReportThreads) {
    report += std::format("… {} more threads\n", summary.threads.size() - kReportThreads);
  }
  report += "\nThreads by wait place\n";
  for (std::size_t i = 0; i < summary.waits.size() && i < kReportWaits; ++i) {
    report += std::format("{:>6}  {}\n", summary.waits[i].second, summary.waits[i].first);
  }
  return report;
}
