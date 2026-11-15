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
  std::vector<std::string> known;
  for (const auto& [name, caps] : m_caps) {
    known.push_back(name);
  }
  runDetached([this, known = std::move(known)]() -> std::function<void()> {
    Snapshot s;
    s.running = printServiceRunning();
    if (s.running) {
      s.printers = listPrinters([&](const std::string& name) { return !std::ranges::contains(known, name); });
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
  for (auto& p : s.printers) {
    if (p.capabilities) {
      m_caps[p.name] = p;
    } else if (const auto it = m_caps.find(p.name); it != m_caps.end()) {
      const PrinterInfo& c = it->second;
      p.duplex = c.duplex;
      p.colour = c.colour;
      p.media = c.media;
      p.options = c.options;
      p.queuedJobs = c.queuedJobs;
      p.capabilities = true;
      // The user's own defaults (lpoptions) arrive without a capability fetch; the printer's come from the cache.
      if (p.defaultMedia.empty()) {
        p.defaultMedia = c.defaultMedia;
      }
      if (p.defaultSides.empty()) {
        p.defaultSides = c.defaultSides;
      }
    }
  }
  std::erase_if(m_errors, [&](const auto& e) { return !std::ranges::contains(s.printers, e.first, &PrinterInfo::name); });
  const Service service = s.running ? Service::Ready : Service::Missing;
  const bool changed = service != m_service || s.printers != m_printers || s.jobs != m_jobs;
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
    if (!queued && !adding && !std::ranges::contains(m_addedUris, f.uri)) {
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
  // CUPS reports no removals on a fresh browse, so a rescan starts empty: printers that left drop off.
  m_found.clear();
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

void PrintersModel::act(const std::string& printer, std::function<std::optional<JobError>()> action,
                        std::function<void()> onSuccess) {
  runDetached([this, printer, action = std::move(action), onSuccess = std::move(onSuccess)]() -> std::function<void()> {
    std::optional<JobError> error = action();
    return [this, printer, error = std::move(error), onSuccess] {
      // The action may change the printer's defaults; fetch its capabilities again.
      m_caps.erase(printer);
      if (error) {
        m_errors[printer] = error->message;
      } else {
        m_errors.erase(printer);
        if (onSuccess) {
          onSuccess();
        }
      }
      m_changed();
      refresh();
    };
  });
}

void PrintersModel::cancelJob(const std::string& printer, int jobId) {
  act(printer, [printer, jobId] { return ::cancelJob(printer, jobId); });
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
      if (m_adding && error) {
        m_adding->error = error->message;
      } else if (m_adding) {
        m_addedUris.push_back(m_adding->printer.uri);
        m_adding.reset();
      }
      m_changed();
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
