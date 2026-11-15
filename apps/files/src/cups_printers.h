#pragma once

#include "print_settings.h"

#include <string>
#include <variant>
#include <vector>

enum class PrinterState { Ready, Printing, NeedsAttention, Offline };

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
};

struct JobError {
  std::string message;
};

// Blocking libcups calls; run them off the UI thread.
[[nodiscard]] std::vector<PrinterInfo> listPrinters();
[[nodiscard]] std::variant<int, JobError> submitJob(const std::string& title, int fd, const PrintOptions& options);
