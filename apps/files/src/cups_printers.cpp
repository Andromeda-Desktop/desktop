#include "cups_printers.h"

#include <cups/cups.h>

#include <algorithm>
#include <cctype>
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

std::string valueText(const char* key, const std::string& value) {
  if (std::strcmp(key, "print-quality") == 0)
    return value == "3" ? "Draft" : value == "5" ? "High" : "Normal";
  std::string out = value;
  std::replace(out.begin(), out.end(), '-', ' ');
  if (!out.empty())
    out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
  return out;
}

std::string attrValue(ipp_attribute_t* a, int i) {
  switch (ippGetValueTag(a)) {
  case IPP_TAG_ENUM:
  case IPP_TAG_INTEGER:
    return std::to_string(ippGetInteger(a, i));
  default:
    return ippGetString(a, i, nullptr) ? ippGetString(a, i, nullptr) : "";
  }
}

void fillOptions(cups_dest_t* dest, cups_dinfo_t* info, PrinterInfo& p) {
  static constexpr std::pair<const char*, const char*> kOptions[] = {
      {"print-quality", "Quality"}, {"media-source", "Paper tray"}, {"media-type", "Paper type"},
      {"output-bin", "Output tray"}};
  for (const auto& [key, label] : kOptions) {
    ipp_attribute_t* supported = cupsFindDestSupported(CUPS_HTTP_DEFAULT, dest, info, key);
    if (!supported || ippGetCount(supported) < 2)
      continue;
    PrinterOption o{.key = key, .label = label};
    for (int i = 0; i < ippGetCount(supported); ++i) {
      std::string v = attrValue(supported, i);
      const char* localized = cupsLocalizeDestValue(CUPS_HTTP_DEFAULT, dest, info, key, v.c_str());
      o.valueLabels.push_back(localized && v != localized ? std::string(localized) : valueText(key, v));
      o.values.push_back(std::move(v));
    }
    if (ipp_attribute_t* def = cupsFindDestDefault(CUPS_HTTP_DEFAULT, dest, info, key)) {
      const auto it = std::ranges::find(o.values, attrValue(def, 0));
      if (it != o.values.end())
        o.defaultIndex = static_cast<std::size_t>(it - o.values.begin());
    }
    p.options.push_back(std::move(o));
  }
}

// The first reason that matters from a comma list ("none", reports and empty ones skipped).
std::string firstStateReason(std::string_view rest) {
  while (!rest.empty()) {
    const auto comma = rest.find(',');
    const std::string_view r = rest.substr(0, comma);
    if (r != "none" && !r.empty() && !r.ends_with("-report"))
      return std::string(r);
    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
  }
  return {};
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
  fillOptions(dest, info, p);
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
    const std::string firstReason = firstStateReason(option(d, "printer-state-reasons"));
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
    if (cupsWriteRequestData(CUPS_HTTP_DEFAULT, buf, static_cast<size_t>(n)) != HTTP_STATUS_CONTINUE) {
      JobError error{cupsLastErrorString()};
      cupsCancelDestJob(CUPS_HTTP_DEFAULT, dest, jobId);
      return finish(std::move(error));
    }
    offset += n;
  }
  if (cupsFinishDestDocument(CUPS_HTTP_DEFAULT, dest, info) != IPP_STATUS_OK) {
    JobError error{cupsLastErrorString()};
    cupsCancelDestJob(CUPS_HTTP_DEFAULT, dest, jobId);
    return finish(std::move(error));
  }
  return finish(jobId);
}

namespace {
// Why a stopped printer does not print (a waiting job carries no printer reasons), or "" when it is not stopped.
std::string stoppedPrinterReason(const std::string& printer) {
  ipp_t* request = ippNewRequest(IPP_OP_GET_PRINTER_ATTRIBUTES);
  char uri[1024];
  httpAssembleURIf(HTTP_URI_CODING_ALL, uri, sizeof uri, "ipp", nullptr, "localhost", 0, "/printers/%s", printer.c_str());
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", nullptr, uri);
  static const char* const kAttrs[] = {"printer-state", "printer-state-reasons", "printer-state-message"};
  ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", 3, nullptr, kAttrs);
  ipp_t* response = cupsDoRequest(CUPS_HTTP_DEFAULT, request, "/");
  if (!response)
    return {};
  std::string out;
  if (ippGetInteger(ippFindAttribute(response, "printer-state", IPP_TAG_ENUM), 0) == IPP_PSTATE_STOPPED) {
    std::string reasons;
    if (ipp_attribute_t* a = ippFindAttribute(response, "printer-state-reasons", IPP_TAG_KEYWORD))
      for (int i = 0; i < ippGetCount(a); ++i)
        reasons += std::string(ippGetString(a, i, nullptr)) + ',';
    const std::string reason = firstStateReason(reasons);
    out = reason.empty() ? std::string("Stopped") : reasonText(reason);
  }
  ippDelete(response);
  return out;
}
} // namespace

JobStatus jobStatus(const std::string& printer, int jobId) {
  ipp_t* request = ippNewRequest(IPP_OP_GET_JOB_ATTRIBUTES);
  const std::string uri = "ipp://localhost/jobs/" + std::to_string(jobId);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "job-uri", nullptr, uri.c_str());
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", nullptr, cupsUser());
  static const char* const kAttrs[] = {"job-state", "job-impressions-completed", "job-printer-state-reasons",
                                       "job-printer-state-message"};
  ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", 4, nullptr, kAttrs);
  ipp_t* response = cupsDoRequest(CUPS_HTTP_DEFAULT, request, "/jobs/");
  JobStatus st;
  if (!response)
    return st;
  if (const ipp_status_t status = ippGetStatusCode(response); status > IPP_STATUS_OK_EVENTS_COMPLETE) {
    // A job CUPS no longer knows (history purged) has ended; other errors leave the state Unknown.
    if (status == IPP_STATUS_ERROR_NOT_FOUND)
      st.state = JobState::Cancelled;
    ippDelete(response);
    return st;
  }
  const int state = ippGetInteger(ippFindAttribute(response, "job-state", IPP_TAG_ENUM), 0);
  st.pagesDone = ippGetInteger(ippFindAttribute(response, "job-impressions-completed", IPP_TAG_INTEGER), 0);
  std::string reasons;
  if (ipp_attribute_t* a = ippFindAttribute(response, "job-printer-state-reasons", IPP_TAG_KEYWORD))
    for (int i = 0; i < ippGetCount(a); ++i)
      reasons += std::string(ippGetString(a, i, nullptr)) + ',';
  const std::string reason = firstStateReason(reasons);
  switch (state) {
  case IPP_JSTATE_PENDING:
  case IPP_JSTATE_HELD:
    st.state = reason.empty() ? JobState::Waiting : JobState::Stopped;
    if (st.state == JobState::Waiting)
      if (std::string stopped = stoppedPrinterReason(printer); !stopped.empty()) {
        st.state = JobState::Stopped;
        st.reason = std::move(stopped);
      }
    break;
  case IPP_JSTATE_PROCESSING:
    st.state = reason.ends_with("-error") ? JobState::Stopped : JobState::Printing;
    break;
  case IPP_JSTATE_STOPPED:
    st.state = JobState::Stopped;
    break;
  case IPP_JSTATE_COMPLETED:
    st.state = JobState::Done;
    break;
  case IPP_JSTATE_CANCELED:
    st.state = JobState::Cancelled;
    break;
  case IPP_JSTATE_ABORTED:
    st.state = JobState::Failed;
    break;
  default:
    break;
  }
  if (!reason.empty())
    st.reason = reasonText(reason);
  ippDelete(response);
  return st;
}

void cancelJob(const std::string& printer, int jobId) { cupsCancelJob2(CUPS_HTTP_DEFAULT, printer.c_str(), jobId, 0); }
