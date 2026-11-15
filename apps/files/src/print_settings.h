#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

enum class Duplex { Off, LongEdge, ShortEdge };
enum class Orientation { Auto, Portrait, Landscape };
enum class Scaling { Fit, Actual, Custom };

// One page range, 1-based and inclusive.
struct PageRange {
  int first = 1;
  int last = 1;
};

struct PrintOptions {
  std::string printer;
  int copies = 1;
  bool collate = true;
  std::optional<std::vector<PageRange>> ranges; // nullopt = all pages
  Duplex duplex = Duplex::Off;
  bool colour = true;
  std::string media; // PWG self-describing name, e.g. "iso_a4_210x297mm"
  int numberUp = 1;
  Orientation orientation = Orientation::Auto;
  Scaling scaling = Scaling::Fit;
  int scalePercent = 100; // used when scaling == Custom
  std::vector<std::pair<std::string, std::string>> printerOptions; // the printer's own IPP options
};

struct RangeError {
  std::string message;
};

// Parses "1-3, 5" against a document of pageCount pages. Returns the ranges or a reason for the user.
[[nodiscard]] std::pair<std::vector<PageRange>, std::optional<RangeError>> parsePageRanges(const std::string& text,
                                                                                         int pageCount);

// IPP job attributes for cupsCreateDestJob.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> cupsJobOptions(const PrintOptions& options);

// Width and height in millimetres from a PWG self-describing media name ("iso_a4_210x297mm", "na_letter_8.5x11in").
[[nodiscard]] std::optional<std::pair<double, double>> mediaSizeMm(const std::string& pwgName);

// A Print request: the sheet is open, the job is being sent (with or without the caller still waiting), or it ended.
enum class PrintStage { SheetOpen, Submitting, CallerGone, Ended };
enum class PrintEvent { UserPrint, UserCancel, CallerClose, JobSent, JobFailed };

struct PrintTransition {
  PrintStage next;
  std::optional<std::uint32_t> reply; // the portal response to send now: 0 printed, 1 cancelled, 2 other
};

// nullopt = the event is not valid in this stage and changes nothing. Each request gets exactly one reply.
[[nodiscard]] std::optional<PrintTransition> printTransition(PrintStage stage, PrintEvent event);
