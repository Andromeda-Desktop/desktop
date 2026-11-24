#include "print_jobs.h"

#include "app/app_notifier.h"
#include "core/blocking_pool.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "i18n/i18n.h"

#include <glib.h>

#include <chrono>
#include <format>
#include <thread>
#include <utility>
#include <vector>

namespace {
  constexpr Logger kLog("print-jobs");
  constexpr auto kPollInterval = std::chrono::milliseconds(1000);

  std::string noticeKey(int jobId) { return "print-" + std::to_string(jobId); }
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

PrintJobs::PrintJobs(AppNotifier& notifier) : m_notifier(notifier) {}

PrintJobs::~PrintJobs() { m_alive.reset(); }

void PrintJobs::track(int jobId, std::string printer, std::string printerLabel, std::string title, int pages) {
  m_jobs[jobId] = Job{.printer = std::move(printer), .label = std::move(printerLabel), .title = std::move(title),
                      .pages = pages};
  apply(jobId, JobStatus{.state = JobState::Waiting});
  if (!m_timer.active())
    m_timer.startRepeating(kPollInterval, [this] { poll(); });
}

void PrintJobs::failed(const std::string& summary, const std::string& body) {
  m_notifier.notify({.title = summary, .body = body, .urgency = 2, .icon = "dialog-warning"}, {}, {});
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
    m_notifier.close(noticeKey(jobId));
    m_jobs.erase(it);
    break;
  }
}

void PrintJobs::show(int jobId, Shown kind, const std::string& summary, const std::string& body) {
  Job& job = m_jobs.at(jobId);
  // A notification the user closed comes back only when the job's state changes kind.
  if (kind == job.shown && (job.dismissed || body == job.body))
    return;
  if (kind != job.shown)
    job.dismissed = false;
  job.shown = kind;
  job.body = body;
  const bool problem = kind == Shown::Problem;
  AppNotice notice{.title = summary, .body = body, .urgency = static_cast<std::uint8_t>(problem ? 2 : 1),
                   .icon = problem ? "dialog-warning" : "printer", .progress = !problem};
  if (problem)
    notice.buttons = {i18n::tr("files.printing.cancel"), i18n::tr("files.printing.queue")};
  const auto alive = std::weak_ptr<bool>(m_alive);
  m_notifier.notify(
      notice, noticeKey(jobId),
      [this, alive, jobId](const std::string& /*token*/, int button) {
        if (alive.expired())
          return;
        const auto it = m_jobs.find(jobId);
        if (it == m_jobs.end())
          return;
        if (button == 1) {
          BlockingPool::instance().run([printer = it->second.printer, jobId] { cancelJob(printer, jobId); });
          m_notifier.close(noticeKey(jobId));
          m_jobs.erase(it);
        } else {
          openPrinterSettings(it->second.printer);
        }
      },
      [this, alive, jobId, kind] {
        if (alive.expired())
          return;
        if (const auto it = m_jobs.find(jobId); it != m_jobs.end() && it->second.shown == kind)
          it->second.dismissed = true;
      });
}
