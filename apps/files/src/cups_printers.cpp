#include "cups_printers.h"

#include <cups/cups.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string_view>
#include <unistd.h>

namespace {

struct DestsDeleter {
  int count;
  void operator()(cups_dest_t* d) const { cupsFreeDests(count, d); }
};

std::string option(const cups_dest_t& d, const char* name) {
  const char* v = cupsGetOption(name, d.num_options, d.options);
  return v ? v : "";
}

std::string reasonText(std::string_view reason) {
  for (const char* suffix : {"-error", "-warning", "-report"})
    if (reason.ends_with(suffix))
      reason.remove_suffix(std::strlen(suffix));
  static constexpr std::pair<std::string_view, const char*> kWords[] = {
      {"media-empty", "Out of paper"},       {"media-needed", "Out of paper"},
      {"media-jam", "Paper jam"},            {"toner-empty", "Out of toner"},
      {"toner-low", "Toner low"},            {"marker-supply-empty", "Out of ink"},
      {"marker-supply-low", "Ink low"},      {"door-open", "A cover is open"},
      {"cover-open", "A cover is open"},     {"offline", "Offline"},
      {"paused", "Paused"},                  {"connecting-to-device", "Connecting"},
      {"input-tray-missing", "A paper tray is missing"},
  };
  for (const auto& [key, words] : kWords)
    if (reason == key)
      return words;
  return std::string(reason);
}

void fillCapabilities(cups_dest_t* dest, PrinterInfo& p) {
  cups_dinfo_t* info = cupsCopyDestInfo(CUPS_HTTP_DEFAULT, dest);
  if (!info)
    return;
  p.duplex = cupsCheckDestSupported(CUPS_HTTP_DEFAULT, dest, info, CUPS_SIDES, CUPS_SIDES_TWO_SIDED_PORTRAIT);
  p.colour = cupsCheckDestSupported(CUPS_HTTP_DEFAULT, dest, info, "print-color-mode", "color");
  const int count = cupsGetDestMediaCount(CUPS_HTTP_DEFAULT, dest, info, CUPS_MEDIA_FLAGS_DEFAULT);
  for (int i = 0; i < count; ++i) {
    cups_size_t size{};
    if (cupsGetDestMediaByIndex(CUPS_HTTP_DEFAULT, dest, info, i, CUPS_MEDIA_FLAGS_DEFAULT, &size) &&
        std::find(p.media.begin(), p.media.end(), size.media) == p.media.end())
      p.media.emplace_back(size.media);
  }
  cups_size_t def{};
  if (cupsGetDestMediaDefault(CUPS_HTTP_DEFAULT, dest, info, CUPS_MEDIA_FLAGS_DEFAULT, &def))
    p.defaultMedia = def.media;
  cupsFreeDestInfo(info);
}

int queuedJobs(const char* name) {
  cups_job_t* jobs = nullptr;
  const int n = cupsGetJobs2(CUPS_HTTP_DEFAULT, &jobs, name, 0, CUPS_WHICHJOBS_ACTIVE);
  cupsFreeJobs(n, jobs);
  return std::max(n, 0);
}

} // namespace

std::vector<PrinterInfo> listPrinters() {
  cups_dest_t* raw = nullptr;
  const int count = cupsGetDests2(CUPS_HTTP_DEFAULT, &raw);
  std::unique_ptr<cups_dest_t, DestsDeleter> dests(raw, DestsDeleter{count});
  std::vector<PrinterInfo> out;
  for (int i = 0; i < count; ++i) {
    cups_dest_t& d = raw[i];
    if (d.instance)
      continue;
    PrinterInfo p;
    p.name = d.name;
    p.label = option(d, "printer-info");
    if (p.label.empty())
      p.label = p.name;
    p.isDefault = d.is_default;
    std::string firstReason;
    const std::string reasons = option(d, "printer-state-reasons");
    for (std::string_view rest = reasons; !rest.empty();) {
      const auto comma = rest.find(',');
      const std::string_view r = rest.substr(0, comma);
      if (r != "none" && !r.empty() && !r.ends_with("-report")) {
        firstReason = std::string(r);
        break;
      }
      rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    }
    const std::string state = option(d, "printer-state"); // IPP: 3 idle, 4 processing, 5 stopped
    if (firstReason.starts_with("offline") || firstReason.starts_with("connecting"))
      p.state = PrinterState::Offline;
    else if (state == "5" || firstReason.ends_with("-error"))
      p.state = PrinterState::NeedsAttention;
    else if (state == "4")
      p.state = PrinterState::Printing;
    else
      p.state = PrinterState::Ready;
    if (!firstReason.empty())
      p.reason = reasonText(firstReason);
    p.queuedJobs = queuedJobs(d.name);
    fillCapabilities(&d, p);
    out.push_back(std::move(p));
  }
  std::stable_sort(out.begin(), out.end(), [](const PrinterInfo& a, const PrinterInfo& b) {
    return a.isDefault > b.isDefault;
  });
  return out;
}

std::variant<int, JobError> submitJob(const std::string& title, int fd, const PrintOptions& o) {
  cups_dest_t* raw = nullptr;
  const int count = cupsGetDests2(CUPS_HTTP_DEFAULT, &raw);
  std::unique_ptr<cups_dest_t, DestsDeleter> dests(raw, DestsDeleter{count});
  cups_dest_t* dest = cupsGetDest(o.printer.c_str(), nullptr, count, raw);
  if (!dest)
    return JobError{"The printer is no longer available."};
  cups_dinfo_t* info = cupsCopyDestInfo(CUPS_HTTP_DEFAULT, dest);
  if (!info)
    return JobError{cupsLastErrorString()};
  int numOptions = 0;
  cups_option_t* options = nullptr;
  for (const auto& [k, v] : cupsJobOptions(o))
    numOptions = cupsAddOption(k.c_str(), v.c_str(), numOptions, &options);
  int jobId = 0;
  const auto finish = [&](std::variant<int, JobError> r) {
    cupsFreeOptions(numOptions, options);
    cupsFreeDestInfo(info);
    return r;
  };
  if (cupsCreateDestJob(CUPS_HTTP_DEFAULT, dest, info, &jobId, title.c_str(), numOptions, options) != IPP_STATUS_OK)
    return finish(JobError{cupsLastErrorString()});
  if (cupsStartDestDocument(CUPS_HTTP_DEFAULT, dest, info, jobId, title.c_str(), CUPS_FORMAT_AUTO, 0, nullptr, 1) !=
      HTTP_STATUS_CONTINUE)
    return finish(JobError{cupsLastErrorString()});
  char buf[65536];
  off_t offset = 0;
  while (true) {
    const ssize_t n = ::pread(fd, buf, sizeof buf, offset);
    if (n < 0) {
      cupsCancelDestJob(CUPS_HTTP_DEFAULT, dest, jobId);
      return finish(JobError{"The document could not be read."});
    }
    if (n == 0)
      break;
    if (cupsWriteRequestData(CUPS_HTTP_DEFAULT, buf, static_cast<size_t>(n)) != HTTP_STATUS_CONTINUE)
      return finish(JobError{cupsLastErrorString()});
    offset += n;
  }
  if (cupsFinishDestDocument(CUPS_HTTP_DEFAULT, dest, info) != IPP_STATUS_OK)
    return finish(JobError{cupsLastErrorString()});
  return finish(jobId);
}
