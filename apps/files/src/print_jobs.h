#pragma once

#include "cups_printers.h"
#include "core/timer_manager.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>

class SessionBus;

// Opens Settings at the printers (Devices until Settings has a Printers page).
void openPrinterSettings();

namespace sdbus {
  class IProxy;
}

// Follows the jobs the Print sheet sent and shows them as notifications (handoff print `notifications`):
// "Printing “<title>”" with the page, and "“<title>” did not print" with the reason, Cancel Job and Show Queue.
class PrintJobs {
public:
  explicit PrintJobs(SessionBus& bus);
  ~PrintJobs();
  PrintJobs(const PrintJobs&) = delete;
  PrintJobs& operator=(const PrintJobs&) = delete;

  void track(int jobId, std::string printer, std::string printerLabel, std::string title, int pages);
  // A job that never reached the queue, or a PDF that was not saved.
  void failed(const std::string& summary, const std::string& body);
  [[nodiscard]] bool idle() const noexcept { return m_jobs.empty(); }

private:
  enum class Poll { Idle, Running };
  enum class Shown { Nothing, Progress, Problem };

  struct Job {
    std::string printer;
    std::string label;
    std::string title;
    int pages = 0;
    std::uint32_t note = 0; // notification id, 0 before the first Notify reply
    Shown shown = Shown::Nothing;
    std::string body;
  };

  void poll();
  void apply(int jobId, const JobStatus& status);
  void show(int jobId, Shown kind, const std::string& summary, const std::string& body);
  void close(std::uint32_t note);

  std::unique_ptr<sdbus::IProxy> m_proxy;
  std::map<int, Job> m_jobs; // by CUPS job id
  std::map<std::uint32_t, int> m_byNote;
  Poll m_poll = Poll::Idle;
  Timer m_timer;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
