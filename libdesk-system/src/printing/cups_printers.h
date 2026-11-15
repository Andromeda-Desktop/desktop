#pragma once

#include "printing/print_settings.h"

#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

enum class PrinterState { Ready, Printing, NeedsAttention, Offline };

// One of the printer's own IPP job options, e.g. print-quality or media-source.
struct PrinterOption {
  std::string key;
  std::string label;
  std::vector<std::string> values;
  std::vector<std::string> valueLabels;
  std::size_t defaultIndex = 0;

  bool operator==(const PrinterOption&) const = default;
};

struct PrinterInfo {
  std::string name; // CUPS destination name
  std::string label; // printer-info, else the name
  std::string uri; // device-uri
  std::string location; // printer-location
  PrinterState state = PrinterState::Offline;
  bool paused = false; // stopped by a person (cupsdisable / Pause), not by an error
  std::string reason; // first printer-state-reason other than "none", in plain words
  int queuedJobs = 0;
  bool isDefault = false;
  bool duplex = false;
  bool colour = false;
  std::vector<std::string> media; // PWG names
  std::string defaultMedia;
  std::string defaultSides; // IPP sides keyword; empty when the printer has no default
  std::vector<PrinterOption> options; // only options with a choice (two or more values)
  bool capabilities = false; // duplex, colour, media and options are filled

  bool operator==(const PrinterInfo&) const = default;
};

struct JobError {
  std::string message;
};

// Blocking libcups calls; run them off the UI thread.
// True when cupsd answers on its socket.
[[nodiscard]] bool printServiceRunning();
// withCapabilities(name) false skips the printer's capability fetch (a network request to the printer) and its
// job count; that PrinterInfo has capabilities false.
[[nodiscard]] std::vector<PrinterInfo> listPrinters(
    const std::function<bool(const std::string&)>& withCapabilities = [](const std::string&) { return true; });
[[nodiscard]] std::variant<int, JobError> submitJob(const std::string& title, int fd, const PrintOptions& options);

// Unknown: CUPS did not answer, so the last state stands. Cancelled ends quietly; Failed tells the user.
enum class JobState { Unknown, Waiting, Printing, Stopped, Done, Cancelled, Failed };

struct JobStatus {
  JobState state = JobState::Unknown;
  int pagesDone = 0;
  std::string reason; // why the job or its printer is stopped, in plain words
};

[[nodiscard]] JobStatus jobStatus(const std::string& printer, int jobId);
std::optional<JobError> cancelJob(const std::string& printer, int jobId);

struct JobInfo {
  int id = 0;
  std::string printer;
  std::string title;
  JobState state = JobState::Waiting; // Waiting, Printing or Stopped
  int pagesDone = 0;

  bool operator==(const JobInfo&) const = default;
};

// Active jobs of every queue, oldest first.
[[nodiscard]] std::vector<JobInfo> listJobs();

// Queue actions. nullopt on success. Pause, resume, add and remove are admin operations: the CUPS default policy
// allows the local admin group over the domain socket.
[[nodiscard]] std::optional<JobError> pausePrinter(const std::string& printer);
[[nodiscard]] std::optional<JobError> resumePrinter(const std::string& printer);
[[nodiscard]] std::optional<JobError> cancelAllJobs(const std::string& printer);
[[nodiscard]] std::optional<JobError> removePrinter(const std::string& printer);
[[nodiscard]] std::optional<JobError> printTestPage(const std::string& printer);
// Per-user (lpoptions), so no admin rights: the default printer ("" for none) and its default paper and sides.
[[nodiscard]] std::optional<JobError> setDefaultPrinter(const std::string& printer);
[[nodiscard]] std::optional<JobError> setPrinterDefault(const std::string& printer, const std::string& key,
                                                        const std::string& value);

// A driverless (IPP Everywhere) printer on the network that is not a queue yet.
struct FoundPrinter {
  std::string label; // make and model, else the URI
  std::string uri;

  bool operator==(const FoundPrinter&) const = default;
};

// DNS-SD browse through CUPS; blocks up to timeoutMs and calls onFound once per printer as it appears.
void discoverPrinters(int timeoutMs, const std::function<void(FoundPrinter)>& onFound);
// Asks an IP address, host name or ipp(s):// URI for its IPP attributes.
[[nodiscard]] std::variant<FoundPrinter, JobError> probePrinter(const std::string& address);
// Creates a driverless queue (ppd-name everywhere), enabled and accepting jobs, under a name no queue has yet.
[[nodiscard]] std::optional<JobError> addPrinter(const FoundPrinter& printer);
