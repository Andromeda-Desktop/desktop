#include "shell/settings/printers_model.h"

#include "core/deferred_call.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace {
  constexpr auto kRefreshInterval = std::chrono::seconds(3);
  constexpr int kDiscoveryMs = 10000;
} // namespace

PrintersModel::PrintersModel(std::function<void()> changed, std::function<bool()> visible)
    : m_changed(std::move(changed)), m_visible(std::move(visible)) {}

void PrintersModel::show() {
  if (!m_timer.active()) {
    refresh();
    m_timer.startRepeating(kRefreshInterval, [this] {
      if (!m_visible()) {
        m_timer.stop();
        return;
      }
      refresh();
    });
  }
  if (m_discovery == Discovery::Idle) {
    discover();
  }
}

void PrintersModel::refresh() {
  if (m_refresh != Refresh::Idle) {
    m_refresh = Refresh::Stale;
    return;
  }
  m_refresh = Refresh::Running;
  runDetached([this]() -> std::function<void()> {
    Snapshot s;
    s.running = printServiceRunning();
    if (s.running) {
      s.printers = listPrinters();
      s.jobs = listJobs();
    }
    return [this, s = std::move(s)]() mutable {
      const bool again = m_refresh == Refresh::Stale;
      m_refresh = Refresh::Idle;
      apply(std::move(s));
      if (again) {
        refresh();
      }
    };
  });
}

void PrintersModel::apply(Snapshot s) {
  const Service service = s.running ? Service::Ready : Service::Missing;
  bool changed = service != m_service || s.printers != m_printers || s.jobs != m_jobs;
  if (m_adding && !m_adding->error &&
      std::ranges::contains(s.printers, m_adding->printer.uri, &PrinterInfo::uri)) {
    m_adding.reset();
    changed = true;
  }
  m_service = service;
  m_printers = std::move(s.printers);
  m_jobs = std::move(s.jobs);
  if (changed) {
    m_changed();
  }
}

std::vector<FoundPrinter> PrintersModel::nearby() const {
  std::vector<FoundPrinter> out;
  for (const auto& f : m_found) {
    const bool queued = std::ranges::contains(m_printers, f.uri, &PrinterInfo::uri);
    const bool adding = m_adding && m_adding->printer.uri == f.uri;
    if (!queued && !adding) {
      out.push_back(f);
    }
  }
  return out;
}

const std::string* PrintersModel::error(const std::string& printer) const {
  const auto it = m_errors.find(printer);
  return it == m_errors.end() ? nullptr : &it->second;
}

void PrintersModel::runDetached(std::function<std::function<void()>()> work) {
  std::thread([alive = std::weak_ptr<void>(m_alive), work = std::move(work)] {
    auto done = work();
    DeferredCall::callLater([alive, done = std::move(done)] {
      if (!alive.expired()) {
        done();
      }
    });
  }).detach();
}

void PrintersModel::discover() {
  if (m_discovery == Discovery::Running) {
    return;
  }
  m_discovery = Discovery::Running;
  m_changed();
  runDetached([this, alive = std::weak_ptr<void>(m_alive)]() -> std::function<void()> {
    discoverPrinters(kDiscoveryMs, [this, alive](FoundPrinter found) {
      DeferredCall::callLater([this, alive, found = std::move(found)] {
        if (!alive.expired() && !std::ranges::contains(m_found, found)) {
          m_found.push_back(found);
          m_changed();
        }
      });
    });
    return [this] {
      m_discovery = Discovery::Done;
      m_changed();
    };
  });
}

void PrintersModel::act(const std::string& printer, std::function<std::optional<JobError>()> action) {
  runDetached([this, printer, action = std::move(action)]() -> std::function<void()> {
    std::optional<JobError> error = action();
    return [this, printer, error = std::move(error)] {
      if (error) {
        m_errors[printer] = error->message;
      } else {
        m_errors.erase(printer);
      }
      m_changed();
      refresh();
    };
  });
}

void PrintersModel::cancelJob(const std::string& printer, int jobId) {
  act(printer, [printer, jobId]() -> std::optional<JobError> {
    ::cancelJob(printer, jobId);
    return std::nullopt;
  });
}

void PrintersModel::add(FoundPrinter printer) {
  if (m_adding && !m_adding->error) {
    return;
  }
  m_adding = Adding{.printer = printer};
  m_changed();
  runDetached([this, printer = std::move(printer)]() -> std::function<void()> {
    std::optional<JobError> error = addPrinter(printer);
    return [this, error = std::move(error)] {
      if (error && m_adding) {
        m_adding->error = error->message;
        m_changed();
      }
      refresh();
    };
  });
}

void PrintersModel::probe(std::string address, std::function<void(std::variant<FoundPrinter, JobError>)> done) {
  runDetached([address = std::move(address), done = std::move(done)]() -> std::function<void()> {
    auto result = probePrinter(address);
    return [done, result = std::move(result)] { done(result); };
  });
}
