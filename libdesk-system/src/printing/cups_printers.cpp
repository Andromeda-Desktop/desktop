#include "printing/cups_printers.h"

#include <cups/cups.h>

#include <cstdlib>

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
  if (ipp_attribute_t* sides = cupsFindDestDefault(CUPS_HTTP_DEFAULT, dest, info, CUPS_SIDES))
    p.defaultSides = attrValue(sides, 0);
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

bool printServiceRunning() {
  http_t* http = httpConnect2(cupsServer(), ippPort(), nullptr, AF_UNSPEC, cupsEncryption(), 1, 1000, nullptr);
  if (!http)
    return false;
  httpClose(http);
  return true;
}

std::vector<PrinterInfo> listPrinters(const std::function<bool(const std::string&)>& withCapabilities) {
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
    p.uri = option(d, "device-uri");
    p.location = option(d, "printer-location");
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
    p.paused = state == "5" && option(d, "printer-state-reasons").find("paused") != std::string::npos;
    if (!firstReason.empty())
      p.reason = reasonText(firstReason);
    // A person who pauses a queue may say why (cupsdisable -r); that beats the bare "Paused".
    if (std::string message = option(d, "printer-state-message"); p.paused && !message.empty())
      p.reason = std::move(message);
    if (withCapabilities(p.name)) {
      p.queuedJobs = queuedJobs(d.name);
      fillCapabilities(&d, p);
      p.capabilities = true;
    }
    // The user's own defaults (lpoptions) win over the printer's.
    if (std::string media = option(d, "media"); !media.empty())
      p.defaultMedia = std::move(media);
    if (std::string sides = option(d, CUPS_SIDES); !sides.empty())
      p.defaultSides = std::move(sides);
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
      HTTP_STATUS_CONTINUE) {
    JobError error{cupsLastErrorString()};
    cupsCancelDestJob(CUPS_HTTP_DEFAULT, dest, jobId);
    return finish(std::move(error));
  }
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

std::optional<JobError> cancelJob(const std::string& printer, int jobId) {
  if (cupsCancelJob2(CUPS_HTTP_DEFAULT, printer.c_str(), jobId, 0) != IPP_STATUS_OK)
    return JobError{cupsLastErrorString()};
  return std::nullopt;
}

std::vector<JobInfo> listJobs() {
  cups_job_t* jobs = nullptr;
  const int n = cupsGetJobs2(CUPS_HTTP_DEFAULT, &jobs, nullptr, 0, CUPS_WHICHJOBS_ACTIVE);
  std::vector<JobInfo> out;
  for (int i = 0; i < n; ++i) {
    const cups_job_t& j = jobs[i];
    JobInfo info{.id = j.id, .printer = j.dest ? j.dest : "", .title = j.title ? j.title : ""};
    if (j.state == IPP_JSTATE_PROCESSING) {
      const JobStatus st = jobStatus(info.printer, j.id);
      info.state = st.state == JobState::Stopped ? JobState::Stopped : JobState::Printing;
      info.pagesDone = st.pagesDone;
    } else {
      info.state = j.state == IPP_JSTATE_STOPPED ? JobState::Stopped : JobState::Waiting;
    }
    out.push_back(std::move(info));
  }
  cupsFreeJobs(n, jobs);
  std::ranges::sort(out, {}, &JobInfo::id);
  return out;
}

namespace {

std::string printerUri(const std::string& printer) {
  char uri[1024];
  httpAssembleURIf(HTTP_URI_CODING_ALL, uri, sizeof uri, "ipp", nullptr, "localhost", 0, "/printers/%s", printer.c_str());
  return uri;
}

std::optional<JobError> lastError() {
  if (cupsLastError() <= IPP_STATUS_OK_EVENTS_COMPLETE)
    return std::nullopt;
  return JobError{cupsLastErrorString()};
}

std::optional<JobError> printerOperation(ipp_op_t op, const std::string& printer) {
  ipp_t* request = ippNewRequest(op);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", nullptr, printerUri(printer).c_str());
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", nullptr, cupsUser());
  ippDelete(cupsDoRequest(CUPS_HTTP_DEFAULT, request, "/admin/"));
  return lastError();
}

// Rewrites the user's lpoptions after edit(dests) changed them.
template <typename Edit> std::optional<JobError> editUserDests(Edit edit) {
  cups_dest_t* raw = nullptr;
  const int count = cupsGetDests2(CUPS_HTTP_DEFAULT, &raw);
  std::unique_ptr<cups_dest_t, DestsDeleter> dests(raw, DestsDeleter{count});
  if (!edit(raw, count))
    return JobError{"The printer is no longer available."};
  if (cupsSetDests2(CUPS_HTTP_DEFAULT, count, raw) != 0)
    return JobError{"Your printer settings could not be saved."};
  return std::nullopt;
}

// A CUPS queue name from a label: letters, digits and '_' only, at most 127 bytes.
std::string queueName(const std::string& label) {
  std::string name;
  for (const char c : label) {
    if (std::isalnum(static_cast<unsigned char>(c)))
      name += c;
    else if (!name.empty() && name.back() != '_')
      name += '_';
  }
  while (!name.empty() && name.back() == '_')
    name.pop_back();
  name.resize(std::min<std::size_t>(name.size(), 127));
  return name.empty() ? std::string("Printer") : name;
}

std::string attrString(ipp_t* response, const char* name) {
  ipp_attribute_t* a = ippFindAttribute(response, name, IPP_TAG_ZERO);
  const char* v = a ? ippGetString(a, 0, nullptr) : nullptr;
  return v ? v : "";
}

} // namespace

std::optional<JobError> pausePrinter(const std::string& printer) { return printerOperation(IPP_OP_PAUSE_PRINTER, printer); }

std::optional<JobError> resumePrinter(const std::string& printer) {
  return printerOperation(IPP_OP_RESUME_PRINTER, printer);
}

std::optional<JobError> removePrinter(const std::string& printer) {
  return printerOperation(IPP_OP_CUPS_DELETE_PRINTER, printer);
}

std::optional<JobError> cancelAllJobs(const std::string& printer) {
  cupsCancelJob2(CUPS_HTTP_DEFAULT, printer.c_str(), CUPS_JOBID_ALL, 0);
  return lastError();
}

std::optional<JobError> printTestPage(const std::string& printer) {
  const char* dataDir = std::getenv("CUPS_DATADIR");
  const std::string file = std::string(dataDir ? dataDir : "/usr/share/cups") + "/data/default-testpage.pdf";
  if (cupsPrintFile2(CUPS_HTTP_DEFAULT, printer.c_str(), file.c_str(), "Test Page", 0, nullptr) == 0)
    return JobError{cupsLastErrorString()};
  return std::nullopt;
}

std::optional<JobError> setDefaultPrinter(const std::string& printer) {
  return editUserDests([&](cups_dest_t* dests, int count) {
    bool found = printer.empty();
    for (int i = 0; i < count; ++i) {
      dests[i].is_default = printer == dests[i].name && !dests[i].instance;
      found = found || dests[i].is_default;
    }
    return found;
  });
}

std::optional<JobError> setPrinterDefault(const std::string& printer, const std::string& key, const std::string& value) {
  return editUserDests([&](cups_dest_t* dests, int count) {
    cups_dest_t* d = cupsGetDest(printer.c_str(), nullptr, count, dests);
    if (!d)
      return false;
    d->num_options = cupsAddOption(key.c_str(), value.c_str(), d->num_options, &d->options);
    return true;
  });
}

void discoverPrinters(int timeoutMs, const std::function<void(FoundPrinter)>& onFound) {
  struct Browse {
    const std::function<void(FoundPrinter)>& onFound;
    std::vector<std::string> seen;
  } browse{onFound, {}};
  cupsEnumDests(
      CUPS_DEST_FLAGS_NONE, timeoutMs, nullptr, CUPS_PRINTER_DISCOVERED, CUPS_PRINTER_DISCOVERED,
      [](void* data, unsigned flags, cups_dest_t* dest) -> int {
        auto& b = *static_cast<Browse*>(data);
        std::string uri = option(*dest, "device-uri");
        if ((flags & CUPS_DEST_FLAGS_REMOVED) || uri.empty() || std::ranges::contains(b.seen, uri))
          return 1;
        std::string label = option(*dest, "printer-make-and-model");
        if (label.empty())
          label = option(*dest, "printer-info");
        b.seen.push_back(uri);
        b.onFound({.label = label.empty() ? uri : std::move(label), .uri = std::move(uri)});
        return 1;
      },
      &browse);
}

std::variant<FoundPrinter, JobError> probePrinter(const std::string& address) {
  const std::string uri = address.find("://") != std::string::npos ? address : "ipp://" + address + "/ipp/print";
  char scheme[32], user[256], host[256], resource[1024];
  int port = 0;
  if (httpSeparateURI(HTTP_URI_CODING_ALL, uri.c_str(), scheme, sizeof scheme, user, sizeof user, host, sizeof host,
                      &port, resource, sizeof resource) < HTTP_URI_STATUS_OK)
    return JobError{"This is not a printer address."};
  const http_encryption_t encryption =
      std::string_view(scheme) == "ipps" ? HTTP_ENCRYPTION_ALWAYS : HTTP_ENCRYPTION_IF_REQUESTED;
  http_t* http = httpConnect2(host, port, nullptr, AF_UNSPEC, encryption, 1, 5000, nullptr);
  if (!http)
    return JobError{"No printer answers at this address."};
  ipp_t* request = ippNewRequest(IPP_OP_GET_PRINTER_ATTRIBUTES);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", nullptr, uri.c_str());
  static const char* const kAttrs[] = {"printer-make-and-model", "printer-info", "document-format-supported"};
  ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", 3, nullptr, kAttrs);
  ipp_t* response = cupsDoRequest(http, request, resource);
  httpClose(http);
  if (!response || ippGetStatusCode(response) > IPP_STATUS_OK_EVENTS_COMPLETE) {
    ippDelete(response);
    return JobError{"No printer answers at this address."};
  }
  bool driverless = false;
  if (ipp_attribute_t* formats = ippFindAttribute(response, "document-format-supported", IPP_TAG_MIMETYPE))
    for (const char* f : {"image/pwg-raster", "image/urf", "application/pdf"})
      driverless = driverless || ippContainsString(formats, f);
  std::string label = attrString(response, "printer-make-and-model");
  if (label.empty())
    label = attrString(response, "printer-info");
  ippDelete(response);
  if (!driverless)
    return JobError{"This printer needs a driver. Andromeda adds only driverless printers."};
  return FoundPrinter{.label = label.empty() ? uri : label, .uri = uri};
}

std::optional<JobError> addPrinter(const FoundPrinter& printer) {
  std::string name = queueName(printer.label);
  {
    cups_dest_t* raw = nullptr;
    const int count = cupsGetDests2(CUPS_HTTP_DEFAULT, &raw);
    std::unique_ptr<cups_dest_t, DestsDeleter> dests(raw, DestsDeleter{count});
    const std::string base = name;
    for (int n = 2; cupsGetDest(name.c_str(), nullptr, count, raw) != nullptr; ++n)
      name = base + "_" + std::to_string(n);
  }
  ipp_t* request = ippNewRequest(IPP_OP_CUPS_ADD_MODIFY_PRINTER);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", nullptr, printerUri(name).c_str());
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", nullptr, cupsUser());
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "ppd-name", nullptr, "everywhere");
  ippAddString(request, IPP_TAG_PRINTER, IPP_TAG_URI, "device-uri", nullptr, printer.uri.c_str());
  ippAddString(request, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-info", nullptr, printer.label.c_str());
  ippAddBoolean(request, IPP_TAG_PRINTER, "printer-is-accepting-jobs", 1);
  ippAddInteger(request, IPP_TAG_PRINTER, IPP_TAG_ENUM, "printer-state", IPP_PSTATE_IDLE);
  ippDelete(cupsDoRequest(CUPS_HTTP_DEFAULT, request, "/admin/"));
  return lastError();
}
