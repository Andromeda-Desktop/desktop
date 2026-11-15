#pragma once

#include "core/timer_manager.h"
#include "printing/cups_printers.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Settings › Printers data: CUPS queues, their jobs and the driverless printers found on the network. CUPS calls
// can wait on a network printer for seconds, so each runs on its own thread, never on the shared worker. Results
// arrive on the main loop; a refresh calls `changed` only when the data differ.
class PrintersModel {
public:
  enum class Service { Unknown, Missing, Ready };
  enum class Discovery { Idle, Running, Done };

  struct Adding {
    FoundPrinter printer;
    std::optional<std::string> error;
  };

  // `visible` tells the refresh timer whether the page still shows; the timer stops when it does not.
  PrintersModel(std::function<void()> changed, std::function<bool()> visible);

  // The page calls this on each build: it starts the refresh timer and, the first time, discovery.
  void show();
  void discover();

  [[nodiscard]] Service service() const noexcept { return m_service; }
  [[nodiscard]] const std::vector<PrinterInfo>& printers() const noexcept { return m_printers; }
  [[nodiscard]] const std::vector<JobInfo>& jobs() const noexcept { return m_jobs; }
  [[nodiscard]] Discovery discovery() const noexcept { return m_discovery; }
  // Found printers that are not queues yet.
  [[nodiscard]] std::vector<FoundPrinter> nearby() const;
  [[nodiscard]] const std::optional<Adding>& adding() const noexcept { return m_adding; }
  // The last failed action on a printer, shown in place of its status line until the next action.
  [[nodiscard]] const std::string* error(const std::string& printer) const;

  // Runs a queue action (pause, resume, cancel, set default, ...) off the main thread, then refreshes.
  void act(const std::string& printer, std::function<std::optional<JobError>()> action);
  void cancelJob(const std::string& printer, int jobId);
  void add(FoundPrinter printer);
  void probe(std::string address, std::function<void(std::variant<FoundPrinter, JobError>)> done);

private:
  struct Snapshot {
    bool running = false;
    std::vector<PrinterInfo> printers;
    std::vector<JobInfo> jobs;
  };

  void refresh();
  void apply(Snapshot snapshot);
  // Runs work on its own thread; the returned callback runs on the main loop while the model lives.
  void runDetached(std::function<std::function<void()>()> work);

  std::function<void()> m_changed;
  std::function<bool()> m_visible;
  std::shared_ptr<void> m_alive = std::make_shared<char>();
  Timer m_timer;
  // Stale: an action finished while a refresh ran, so its result may predate the action; refresh once more.
  enum class Refresh { Idle, Running, Stale } m_refresh = Refresh::Idle;
  Service m_service = Service::Unknown;
  Discovery m_discovery = Discovery::Idle;
  std::vector<PrinterInfo> m_printers;
  std::vector<JobInfo> m_jobs;
  std::vector<FoundPrinter> m_found;
  std::optional<Adding> m_adding;
  std::map<std::string, std::string> m_errors;
};
