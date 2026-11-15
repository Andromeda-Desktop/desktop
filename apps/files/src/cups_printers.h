#pragma once

#include "print_settings.h"

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
};

struct PrinterInfo {
  std::string name; // CUPS destination name
  std::string label; // printer-info, else the name
  PrinterState state = PrinterState::Offline;
  std::string reason; // first printer-state-reason other than "none", in plain words
  int queuedJobs = 0;
  bool isDefault = false;
  bool duplex = false;
  bool colour = false;
  std::vector<std::string> media; // PWG names
  std::string defaultMedia;
  std::vector<PrinterOption> options; // only options with a choice (two or more values)
};

struct JobError {
  std::string message;
};

// Blocking libcups calls; run them off the UI thread.
[[nodiscard]] std::vector<PrinterInfo> listPrinters();
[[nodiscard]] std::variant<int, JobError> submitJob(const std::string& title, int fd, const PrintOptions& options);

enum class JobState { Waiting, Printing, Stopped, Done, Failed };

struct JobStatus {
  JobState state = JobState::Failed;
  int pagesDone = 0;
  std::string reason; // why the job or its printer is stopped, in plain words
};

[[nodiscard]] JobStatus jobStatus(const std::string& printer, int jobId);
void cancelJob(const std::string& printer, int jobId);
