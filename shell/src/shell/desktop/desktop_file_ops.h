#pragma once

#include "core/timer_manager.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class SessionBus;

namespace sdbus {
  class IProxy;
  class Variant;
}

// Copies, moves, trash and deletes of the shell (desktop, dock Trash) as org.andromeda.FileOps1 jobs, so they have undo
// like Files (FILES-UI-SPEC §20). The shell has no conflict sheet: a name clash keeps both. The daemon is D-Bus
// activated.
class DesktopFileOps {
public:
  enum class Kind : std::uint8_t { Copy, Move, Trash, Delete, Undo };
  // The oldest job that has run for a while, for the live activity.
  struct Progress {
    Kind kind = Kind::Copy;
    std::size_t items = 0;
    std::string current; // file name the job works on now
    float fraction = 0.0F;
  };

  explicit DesktopFileOps(SessionBus* bus);
  ~DesktopFileOps();
  DesktopFileOps(const DesktopFileOps&) = delete;
  DesktopFileOps& operator=(const DesktopFileOps&) = delete;

  // One job for the batch; false when the bus is missing or the call cannot be sent.
  bool transfer(bool move, const std::vector<std::filesystem::path>& sources, const std::filesystem::path& destination);
  bool trash(const std::vector<std::filesystem::path>& paths);
  // Deletes for good, as one job that cannot be undone.
  bool remove(const std::vector<std::filesystem::path>& paths);
  // Undoes the newest finished job that can be undone; false when there is none.
  bool undo();

  [[nodiscard]] std::optional<Progress> progress() const;
  void cancelProgress();
  // Runs when progress() can have changed.
  void setOnProgress(std::function<void()> callback) { m_onProgress = std::move(callback); }
  // Runs once per failed job with the daemon's message.
  void setOnError(std::function<void(Kind kind, const std::string& message)> callback) {
    m_onError = std::move(callback);
  }

private:
  enum class Phase : std::uint8_t { Running, Finishing };
  struct Job {
    std::unique_ptr<sdbus::IProxy> proxy;
    Kind kind = Kind::Copy;
    std::size_t items = 0;
    Phase phase = Phase::Running;
    std::chrono::steady_clock::time_point started;
    Timer reveal; // asks for progress() once the job is old enough to show, also when it reports nothing
    std::uint64_t doneBytes = 0;
    std::uint64_t totalBytes = 0;
    std::uint64_t doneFiles = 0;
    std::uint64_t totalFiles = 0;
    std::string current;
    std::string error;
  };

  [[nodiscard]] const Job* shown() const; // the job progress() reports
  bool start(Kind kind, const std::vector<std::filesystem::path>& sources,
             const std::optional<std::filesystem::path>& destination);
  void track(const std::string& path, Kind kind, std::size_t items);
  void apply(const std::string& path, const std::map<std::string, sdbus::Variant>& changed);
  void finish(const std::string& path, const std::string& state, bool undoable);

  SessionBus* m_bus = nullptr;
  std::unique_ptr<sdbus::IProxy> m_manager;
  std::map<std::string, Job> m_jobs; // by object path
  std::vector<std::string> m_undo;   // newest last
  std::function<void()> m_onProgress;
  std::function<void(Kind, const std::string&)> m_onError;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
