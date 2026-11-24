#include "shell/desktop/desktop_file_ops.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "dbus/session_bus.h"
#include "dbus/reply_log.h"

#include <cstdint>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>

namespace {
  constexpr Logger kLog("desktop-icons");
  const sdbus::ServiceName kName{"org.andromeda.FileOps1"};
  const sdbus::ObjectPath kPath{"/org/andromeda/FileOps1"};
  constexpr auto kInterface = "org.andromeda.FileOps1";
  constexpr auto kJobInterface = "org.andromeda.FileOps1.Job";
  constexpr auto kProperties = "org.freedesktop.DBus.Properties";
  constexpr std::size_t kUndoDepth = 50;
  // A job shorter than this never shows progress, so a quick copy does not flash the island.
  constexpr auto kShowAfter = std::chrono::milliseconds(800);

  using Bytes = std::vector<std::uint8_t>;
  using Properties = std::map<std::string, sdbus::Variant>;

  Bytes bytes(const std::filesystem::path& path) {
    const std::string& s = path.native();
    return {s.begin(), s.end()};
  }

  bool finished(const std::string& state) { return state == "done" || state == "failed" || state == "cancelled"; }

  const char* methodOf(DesktopFileOps::Kind kind) {
    switch (kind) {
    case DesktopFileOps::Kind::Copy:
      return "Copy";
    case DesktopFileOps::Kind::Move:
      return "Move";
    case DesktopFileOps::Kind::Trash:
      return "Trash";
    case DesktopFileOps::Kind::Delete:
      return "Delete";
    case DesktopFileOps::Kind::Undo:
      return "Undo";
    }
    return "";
  }
} // namespace

DesktopFileOps::DesktopFileOps(SessionBus* bus) : m_bus(bus) {
  if (m_bus != nullptr) {
    m_manager = sdbus::createProxy(m_bus->connection(), kName, kPath);
  }
}

DesktopFileOps::~DesktopFileOps() = default;

bool DesktopFileOps::transfer(bool move, const std::vector<std::filesystem::path>& sources,
                              const std::filesystem::path& destination) {
  return start(move ? Kind::Move : Kind::Copy, sources, destination);
}

bool DesktopFileOps::trash(const std::vector<std::filesystem::path>& paths) {
  return start(Kind::Trash, paths, std::nullopt);
}

bool DesktopFileOps::remove(const std::vector<std::filesystem::path>& paths) {
  return start(Kind::Delete, paths, std::nullopt);
}

bool DesktopFileOps::start(Kind kind, const std::vector<std::filesystem::path>& sources,
                           const std::optional<std::filesystem::path>& destination) {
  if (m_manager == nullptr || sources.empty()) {
    return false;
  }
  std::vector<Bytes> list;
  for (const auto& source : sources) {
    list.push_back(bytes(source));
  }
  const auto reply = [this, alive = std::weak_ptr<bool>(m_alive), kind,
                      count = sources.size()](std::optional<sdbus::Error> error, sdbus::ObjectPath job) {
    if (alive.expired()) {
      return;
    }
    if (error.has_value()) {
      kLog.warn("FileOps1 {} failed: {}", methodOf(kind), error->getMessage());
      if (m_onError) {
        m_onError(kind, error->getMessage());
      }
      return;
    }
    kLog.debug("desktop {} of {} items as {}", methodOf(kind), count, std::string(job));
    track(job, kind, count);
  };
  try {
    auto call = m_manager->callMethodAsync(methodOf(kind)).onInterface(kInterface);
    if (destination.has_value()) {
      call.withArguments(list, bytes(*destination)).uponReplyInvoke(reply);
    } else {
      call.withArguments(list).uponReplyInvoke(reply);
    }
  } catch (const sdbus::Error& e) {
    kLog.warn("FileOps1 is not reachable: {}", e.getMessage());
    return false;
  }
  return true;
}

void DesktopFileOps::track(const std::string& path, Kind kind, std::size_t items) {
  auto proxy = sdbus::createProxy(m_bus->connection(), kName, sdbus::ObjectPath{path});
  proxy->uponSignal("PropertiesChanged")
      .onInterface(kProperties)
      .call([this, path](const std::string& iface, const Properties& changed, const std::vector<std::string>&) {
        if (iface == kJobInterface) {
          apply(path, changed);
        }
      });
  auto* raw = proxy.get();
  Job& job = m_jobs[path] = Job{.proxy = std::move(proxy), .kind = kind, .items = items,
                                .started = std::chrono::steady_clock::now()};
  job.reveal.start(kShowAfter, [this] {
    if (m_onProgress) {
      m_onProgress();
    }
  });
  // The job may have asked or finished before the signal was wired: read its state once.
  raw->callMethodAsync("GetAll")
      .onInterface(kProperties)
      .withArguments(std::string(kJobInterface))
      .uponReplyInvoke([this, alive = std::weak_ptr<bool>(m_alive), path](std::optional<sdbus::Error> error,
                                                                           Properties all) {
        if (alive.expired() || !m_jobs.contains(path)) {
          return;
        }
        if (error.has_value()) {
          m_jobs.erase(path); // gone already (daemon restart): nothing to follow
          return;
        }
        apply(path, all);
      });
}

void DesktopFileOps::apply(const std::string& path, const Properties& changed) {
  const auto it = m_jobs.find(path);
  if (it == m_jobs.end() || it->second.phase != Phase::Running) {
    return;
  }
  Job& job = it->second;
  const auto read = [&](const char* name, auto& out) {
    if (const auto value = changed.find(name); value != changed.end()) {
      out = value->second.get<std::remove_reference_t<decltype(out)>>();
    }
  };
  read("DoneBytes", job.doneBytes);
  read("TotalBytes", job.totalBytes);
  read("DoneFiles", job.doneFiles);
  read("TotalFiles", job.totalFiles);
  read("Error", job.error);
  if (const auto current = changed.find("Current"); current != changed.end()) {
    const auto name = current->second.get<Bytes>();
    job.current = std::filesystem::path(std::string(name.begin(), name.end())).filename().string();
  }
  const auto state = changed.find("State");
  if (state == changed.end()) {
    if (m_onProgress) {
      m_onProgress();
    }
    return;
  }
  const auto value = state->second.get<std::string>();
  if (value == "conflict") {
    // No conflict sheet in the shell: keep both, as Finder names a dropped clash "name 2".
    job.proxy->callMethodAsync("ResolveConflict")
        .onInterface(kJobInterface)
        .withArguments(std::string("keep_both"), true)
        .uponReplyInvoke(logReply(kLog, "ResolveConflict"));
  } else if (finished(value)) {
    const auto undoable = changed.find("Undoable");
    finish(path, value, undoable != changed.end() && undoable->second.get<bool>());
  }
}

void DesktopFileOps::finish(const std::string& path, const std::string& state, bool undoable) {
  Job& job = m_jobs.at(path);
  job.phase = Phase::Finishing; // the signal and the first GetAll can both report the end
  if (state == "failed") {
    kLog.warn("desktop {} job {} failed: {}", methodOf(job.kind), path, job.error);
    if (m_onError) {
      m_onError(job.kind, job.error);
    }
  }
  if (m_onProgress) {
    m_onProgress();
  }
  const bool remember = state == "done" && job.kind != Kind::Delete && job.kind != Kind::Undo;
  // The daemon signals State before Undoable: read Undoable once more when it was not in the same signal.
  const auto settle = [this, path](bool canUndo) {
    kLog.debug("desktop job {} finished, undoable {}", path, canUndo);
    if (canUndo) {
      m_undo.push_back(path);
      if (m_undo.size() > kUndoDepth) {
        m_undo.erase(m_undo.begin());
      }
    }
    // Not from inside this proxy's own callback.
    DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive), path] {
      if (!alive.expired()) {
        m_jobs.erase(path);
      }
    });
  };
  if (!remember || undoable) {
    settle(remember);
    return;
  }
  job.proxy->getPropertyAsync("Undoable")
      .onInterface(kJobInterface)
      .uponReplyInvoke([alive = std::weak_ptr<bool>(m_alive), settle](std::optional<sdbus::Error> error,
                                                                       sdbus::Variant value) {
        if (!alive.expired()) {
          settle(!error.has_value() && value.get<bool>());
        }
      });
}

const DesktopFileOps::Job* DesktopFileOps::shown() const {
  const auto now = std::chrono::steady_clock::now();
  const Job* oldest = nullptr;
  for (const auto& [path, job] : m_jobs) {
    if (job.phase == Phase::Running && now - job.started >= kShowAfter
        && (oldest == nullptr || job.started < oldest->started)) {
      oldest = &job;
    }
  }
  return oldest;
}

std::optional<DesktopFileOps::Progress> DesktopFileOps::progress() const {
  const Job* job = shown();
  if (job == nullptr) {
    return std::nullopt;
  }
  // Bytes when there are any; a tree of empty files moves by its file count.
  const auto [done, total] = job->totalBytes > 0 ? std::pair{job->doneBytes, job->totalBytes}
                                                 : std::pair{job->doneFiles, job->totalFiles};
  const double fraction = total == 0 ? 0.0 : static_cast<double>(done) / static_cast<double>(total);
  return Progress{.kind = job->kind, .items = job->items, .current = job->current, .fraction = static_cast<float>(fraction)};
}

void DesktopFileOps::cancelProgress() {
  if (const Job* job = shown(); job != nullptr) {
    job->proxy->callMethodAsync("Cancel").onInterface(kJobInterface).uponReplyInvoke(logReply(kLog, "Cancel", ReplyFailure::Debug));
  }
}

bool DesktopFileOps::undo() {
  if (m_manager == nullptr || m_undo.empty()) {
    return false;
  }
  const std::string job = m_undo.back();
  m_undo.pop_back();
  try {
    m_manager->callMethodAsync("Undo")
        .onInterface(kInterface)
        .withArguments(sdbus::ObjectPath{job})
        .uponReplyInvoke([this, alive = std::weak_ptr<bool>(m_alive), job](std::optional<sdbus::Error> error,
                                                                             sdbus::ObjectPath undoJob) {
          if (alive.expired()) {
            return;
          }
          if (error.has_value()) {
            // The daemon quits after 10 min idle and forgets its jobs, and Files may have undone this one: try the
            // next older entry.
            kLog.debug("undo of {} not possible: {}", job, error->getMessage());
            undo();
            return;
          }
          kLog.debug("desktop undo {} as {}", job, std::string(undoJob));
          // Followed so a clash during the undo keeps both instead of waiting for an answer; not undoable again.
          track(std::string(undoJob), Kind::Undo, 0);
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("undo of {} failed: {}", job, e.getMessage());
    return false;
  }
  return true;
}
