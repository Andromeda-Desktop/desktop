#pragma once

#include "core/poll_source.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// inotify on directories, one fd per process (a PollSource on the app loop). A rename inside one watched directory
// arrives as Renamed; a move out of or into it as Deleted or MovedIn. On macOS (kqueue) every change is Overflow.
class DirectoryWatcher final : public PollSource {
public:
  enum class Event : std::uint8_t {
    Created,  // a new name (IN_CREATE): its content may still be on the way
    MovedIn,  // moved in from outside the directory, content complete
    Deleted,
    Changed,  // content or attributes
    Written,  // a writer closed it
    Renamed,  // name -> newName
    Gone,     // the watched directory itself was deleted or moved away
    Overflow, // the kernel dropped events: rescan
  };
  using Callback = std::function<void(Event event, const std::string& name, const std::string& newName)>;
  using WatchId = int;
  enum class Follow : std::uint8_t {
    Inode, // the watch ends with the directory: Gone is the last event
    Path,  // a missing, deleted or moved-away directory is watched again once the path exists, then Overflow
           // (rescan); a rename of an ancestor moves the watch with it
  };

  DirectoryWatcher();
  ~DirectoryWatcher() override;

  DirectoryWatcher(const DirectoryWatcher&) = delete;
  DirectoryWatcher& operator=(const DirectoryWatcher&) = delete;

  // -1 when the directory cannot be watched (no inotify, no permission, watch limit); with Follow::Inode also when
  // it does not exist.
  WatchId watch(const std::filesystem::path& dir, Callback callback, Follow follow = Follow::Inode);
  void unwatch(WatchId id);

  [[nodiscard]] int pollTimeoutMs() const override;
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;
  // Reads and delivers what is queued; for loops that poll the fd themselves.
  void processEvents();
  [[nodiscard]] int fd() const noexcept { return m_fd; }

private:
  void doAddPollFds(std::vector<pollfd>& fds) override;
  // Delivers Deleted for moves out whose pair time passed.
  void flushPendingMoves();
  void subscribe(WatchId id, int wd, Callback callback);
  void detach(WatchId id);
  // A followed id moves between wds: a copied subscriber list may hold it under its old wd.
  [[nodiscard]] bool subscribed(WatchId id, int wd) const {
    const auto it = m_idToWd.find(id);
    return it != m_idToWd.end() && it->second == wd;
  }
  // Watches the followed path, or its nearest existing ancestor until the path exists; `announce` sends Overflow
  // when the path itself is watched again.
  void follow(WatchId id, bool announce);

  struct Watch {
    std::vector<std::pair<int, Callback>> subscribers;
  };
  struct PendingMove {
    int wd;
    std::uint32_t cookie;
    std::string name;
    std::chrono::steady_clock::time_point deadline;
  };

  int m_fd = -1;
  int m_nextId = 1;
  std::unordered_map<int, Watch> m_watches;       // by inotify wd
  std::unordered_map<int, int> m_idToWd;          // by WatchId
  std::vector<PendingMove> m_pendingMoves;        // IN_MOVED_FROM waiting for a matching IN_MOVED_TO
  struct Followed {
    std::filesystem::path path;
    Callback callback;
  };
  std::unordered_map<int, Followed> m_followed;   // by WatchId, Follow::Path only
};
