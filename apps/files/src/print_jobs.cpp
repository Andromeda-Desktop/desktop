#include "print_jobs.h"

#include "core/blocking_pool.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "dbus/session_bus.h"
#include "dbus/reply_log.h"
#include "i18n/i18n.h"

#include <glib.h>
#include <sdbus-c++/sdbus-c++.h>

#include <chrono>
#include <format>
#include <thread>
#include <utility>
#include <vector>

namespace {
  constexpr Logger kLog("print-jobs");
  constexpr auto kInterface = "org.freedesktop.Notifications";
  constexpr auto kPollInterval = std::chrono::milliseconds(1000);
} // namespace

void openPrinterSettings(const std::string& printer) {
    const std::string context = printer.empty() ? "printers" : "printer:" + printer;
    const gchar* argv[] = {"andromeda-shell", "msg", "settings-open", context.c_str(), nullptr};
    GError* error = nullptr;
    if (!g_spawn_async(nullptr, const_cast<gchar**>(argv), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr,
                       &error)) {
      kLog.warn("open printers: {}", error != nullptr ? error->message : "?");
      g_clear_error(&error);
    }
}

PrintJobs::PrintJobs(SessionBus& bus) {
  if (!bus.connected())
    return;
  try {
    m_proxy = sdbus::createProxy(bus.connection(), sdbus::ServiceName{kInterface},
                                 sdbus::ObjectPath{"/org/freedesktop/Notifications"});
    m_proxy->uponSignal("ActionInvoked").onInterface(kInterface).call([this](std::uint32_t note, const std::string& key) {
      const auto it = m_byNote.find(note);
      if (it == m_byNote.end())
        return;
      const auto job = m_jobs.find(it->second);
      if (job == m_jobs.end())
        return;
      if (key == "cancel") {
        BlockingPool::instance().run([printer = job->second.printer, id = job->first] { cancelJob(printer, id); });
        close(note);
        m_byNote.erase(it);
        m_jobs.erase(job);
      } else if (key == "queue") {
        openPrinterSettings(job->second.printer);
      }
    });
    m_proxy->uponSignal("NotificationClosed").onInterface(kInterface).call([this](std::uint32_t note, std::uint32_t) {
      const auto it = m_byNote.find(note);
      if (it == m_byNote.end())
        return;
      if (const auto job = m_jobs.find(it->second); job != m_jobs.end())
        job->second.note = 0;
      m_byNote.erase(it);
    });
  } catch (const sdbus::Error& e) {
    kLog.warn("notifications unavailable: {}", e.what());
    m_proxy.reset();
  }
}

PrintJobs::~PrintJobs() { *m_alive = false; }

void PrintJobs::track(int jobId, std::string printer, std::string printerLabel, std::string title, int pages) {
  m_jobs[jobId] = Job{.printer = std::move(printer), .label = std::move(printerLabel), .title = std::move(title),
                      .pages = pages};
  apply(jobId, JobStatus{.state = JobState::Waiting});
  if (!m_timer.active())
    m_timer.startRepeating(kPollInterval, [this] { poll(); });
}

void PrintJobs::failed(const std::string& summary, const std::string& body) {
  if (!m_proxy)
    return;
  const std::map<std::string, sdbus::Variant> hints{
      {"desktop-entry", sdbus::Variant{std::string{"dev.andromeda.Files"}}},
      {"urgency", sdbus::Variant{std::uint8_t{2}}}};
  try {
    m_proxy->callMethodAsync("Notify")
        .onInterface(kInterface)
        .withArguments(std::string{"Files"}, std::uint32_t{0}, std::string{"dialog-warning"}, summary, body,
                       std::vector<std::string>{}, hints, std::int32_t{-1})
        .uponReplyInvoke([](std::optional<sdbus::Error> error, std::uint32_t) {
          if (error)
            kLog.warn("notify: {}", error->what());
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("notify: {}", e.what());
  }
}

void PrintJobs::poll() {
  if (m_jobs.empty()) {
    m_timer.stop();
    return;
  }
  if (m_poll == Poll::Running)
    return;
  m_poll = Poll::Running;
  std::vector<int> ids;
  std::map<int, std::string> printers;
  for (const auto& [id, job] : m_jobs) {
    ids.push_back(id);
    printers[id] = job.printer;
  }
  BlockingPool::instance().run([this, ids = std::move(ids), printers = std::move(printers), alive = std::weak_ptr<bool>(m_alive)] {
    std::vector<std::pair<int, JobStatus>> statuses;
    for (const int id : ids)
      statuses.emplace_back(id, jobStatus(printers.at(id), id));
    DeferredCall::callLater([this, statuses = std::move(statuses), alive] {
      if (alive.expired())
        return;
      m_poll = Poll::Idle;
      for (const auto& [id, status] : statuses)
        apply(id, status);
    });
  });
}

void PrintJobs::apply(int jobId, const JobStatus& status) {
  const auto it = m_jobs.find(jobId);
  if (it == m_jobs.end())
    return;
  Job& job = it->second;
  switch (status.state) {
  case JobState::Waiting:
    show(jobId, Shown::Progress, i18n::tr("files.printing.title", "title", job.title), i18n::tr("files.printing.waiting", "printer", job.label));
    break;
  case JobState::Printing:
    show(jobId, Shown::Progress, i18n::tr("files.printing.title", "title", job.title),
         job.pages > 0 ? i18n::tr("files.printing.page", "printer", job.label, "page", std::min(status.pagesDone + 1, job.pages), "pages", job.pages)
                       : job.label);
    break;
  case JobState::Stopped:
    show(jobId, Shown::Problem, i18n::tr("files.printing.failed-title", "title", job.title),
         i18n::tr("files.printing.stopped", "printer", job.label, "reason", status.reason.empty() ? i18n::tr("files.printing.stopped-reason") : status.reason));
    break;
  case JobState::Unknown:
    break;
  case JobState::Done:
  case JobState::Cancelled:
  case JobState::Failed:
    if (status.state == JobState::Failed)
      failed(i18n::tr("files.printing.failed-title", "title", job.title),
             i18n::tr("files.printing.failed", "printer", job.label, "reason", status.reason.empty() ? i18n::tr("files.printing.failed-reason") : status.reason));
    // A Notify still in flight closes its notification in the reply, as the job is gone.
    if (job.note != 0) {
      m_byNote.erase(job.note);
      close(job.note);
    }
    m_jobs.erase(it);
    break;
  }
}

void PrintJobs::show(int jobId, Shown kind, const std::string& summary, const std::string& body) {
  Job& job = m_jobs.at(jobId);
  // A notification the user closed comes back only when the job's state changes kind.
  if (kind == job.shown && ((job.note == 0 && job.sending == Sending::Idle) || body == job.body))
    return;
  job.shown = kind;
  job.summary = summary;
  job.body = body;
  if (job.sending != Sending::Idle) {
    job.sending = Sending::Stale;
    return;
  }
  send(jobId);
}

void PrintJobs::send(int jobId) {
  Job& job = m_jobs.at(jobId);
  if (!m_proxy)
    return;
  const bool problem = job.shown == Shown::Problem;
  const std::vector<std::string> actions =
      problem ? std::vector<std::string>{"cancel", i18n::tr("files.printing.cancel"), "queue", i18n::tr("files.printing.queue")} : std::vector<std::string>{};
  const std::map<std::string, sdbus::Variant> hints{
      {"desktop-entry", sdbus::Variant{std::string{"dev.andromeda.Files"}}},
      {"urgency", sdbus::Variant{static_cast<std::uint8_t>(problem ? 2 : 1)}},
      {"transient", sdbus::Variant{!problem}}};
  try {
    // One Notify in flight per job, so each update replaces the notification instead of adding a second one.
    job.sending = Sending::InFlight;
    m_proxy->callMethodAsync("Notify")
        .onInterface(kInterface)
        .withArguments(std::string{"Files"}, job.note, std::string{problem ? "dialog-warning" : "printer"}, job.summary,
                       job.body, actions, hints, std::int32_t{0})
        .uponReplyInvoke([this, jobId, alive = std::weak_ptr<bool>(m_alive)](std::optional<sdbus::Error> error,
                                                                             std::uint32_t note) {
          if (alive.expired())
            return;
          const auto it = m_jobs.find(jobId);
          if (it == m_jobs.end()) {
            close(note);
            return;
          }
          Job& job = it->second;
          const bool stale = job.sending == Sending::Stale;
          job.sending = Sending::Idle;
          if (error) {
            kLog.warn("notify: {}", error->what());
            return;
          }
          if (job.note != note)
            m_byNote.erase(job.note);
          job.note = note;
          m_byNote[note] = jobId;
          if (stale)
            send(jobId);
        });
  } catch (const sdbus::Error& e) {
    job.sending = Sending::Idle;
    kLog.warn("notify: {}", e.what());
  }
}

void PrintJobs::close(std::uint32_t note) {
  if (!m_proxy || note == 0)
    return;
  try {
    m_proxy->callMethodAsync("CloseNotification")
        .onInterface(kInterface)
        .withArguments(note)
        .uponReplyInvoke(logReply(kLog, "CloseNotification", ReplyFailure::Debug));
  } catch (const sdbus::Error& e) {
    kLog.warn("close notification: {}", e.what());
  }
}
