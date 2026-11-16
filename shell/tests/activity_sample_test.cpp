#include "system/activity_sample.h"
#include "tests/test_check.h"

#include <string>
#include <unistd.h>

int main() {
  ThreadSnapshot parsed;
  // A comm with spaces and ')' must not shift the fields.
  TEST_CHECK(parseTaskStat("42 (Web (Content) x) S 1 2 3 4 5 6 7 8 9 10 120 30 0 0 20 0 1 0", parsed));
  TEST_CHECK(parsed.name == "Web (Content) x");
  TEST_CHECK(parsed.state == 'S');
  TEST_CHECK(parsed.ticks == 150);
  TEST_CHECK(!parseTaskStat("42 (truncated) S 1 2", parsed));

  // The busiest thread over the interval leads, not the one with the most total time; a new thread counts from 0.
  const std::vector<ThreadSnapshot> before{
      {.pid = 10, .tid = 10, .name = "main", .state = 'S', .ticks = 5000, .wchan = "ep_poll"},
      {.pid = 10, .tid = 11, .name = "worker", .state = 'R', .ticks = 100},
  };
  const std::vector<ThreadSnapshot> after{
      {.pid = 10, .tid = 10, .name = "main", .state = 'S', .ticks = 5010, .wchan = "ep_poll"},
      {.pid = 10, .tid = 11, .name = "worker", .state = 'R', .ticks = 500},
      {.pid = 12, .tid = 12, .name = "helper", .state = 'D', .ticks = 50, .wchan = "folio_wait_bit"},
  };
  const SampleSummary summary = summarizeSample("Test", before, after, 5.0, 100, 0);
  TEST_CHECK(summary.threads.front().name == "worker" && summary.threads.front().wait == "–");
  TEST_CHECK(summary.waits.size() == 3);
  const std::string report = sampleReport(summary);
  TEST_CHECK(report.find("2 processes, 3 threads, 92.0 % CPU") != std::string::npos);
  TEST_CHECK(report.find("80.0  R      worker") != std::string::npos);
  TEST_CHECK(report.find("worker") < report.find("helper"));
  TEST_CHECK(report.find("helper") < report.find("main"));
  TEST_CHECK(report.find("10.0  D      helper") != std::string::npos);
  TEST_CHECK(report.find("folio_wait_bit") != std::string::npos);

  // This process can sample itself.
  const auto self = snapshotThreads({static_cast<int>(::getpid())});
  TEST_CHECK(!self.empty() && self.front().pid == static_cast<int>(::getpid()));
  return 0;
}
