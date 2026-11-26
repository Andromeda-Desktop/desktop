#include "core/files/directory_watcher.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <sys/inotify.h>
#include <unistd.h>

namespace {
  constexpr Logger kLog("dir-watch");
  constexpr std::uint32_t kMask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_MOVED_FROM
      | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_EXCL_UNLINK;
  // A followed path's ancestor reports only arrivals and its own end; IN_MASK_ADD keeps a wider mask another watch of
  // the same directory set.
  constexpr std::uint32_t kAncestorMask = IN_CREATE | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR
      | IN_MASK_ADD;
  // A MOVED_FROM whose MOVED_TO does not come within this time was a move out of the directory.
  constexpr int kMovePairMs = 20;
} // namespace

DirectoryWatcher::DirectoryWatcher() : m_fd(inotify_init1(IN_NONBLOCK | IN_CLOEXEC)) {
  if (m_fd < 0) {
    kLog.warn("inotify unavailable: {}", std::strerror(errno));
  }
}

DirectoryWatcher::~DirectoryWatcher() {
  if (m_fd >= 0) {
    ::close(m_fd);
  }
}

DirectoryWatcher::WatchId DirectoryWatcher::watch(const std::filesystem::path& dir, Callback callback, Follow follow) {
  if (m_fd < 0) {
    return -1;
  }
  const int id = m_nextId++;
  if (follow == Follow::Path) {
    std::error_code ec;
    auto path = std::filesystem::absolute(dir, ec);
    if (ec || dir.empty()) {
      kLog.warn("cannot watch '{}': {}", dir.string(), ec ? ec.message() : "empty path");
      return -1;
    }
    m_followed.emplace(id, Followed{.path = std::move(path), .callback = std::move(callback)});
    this->follow(id, false);
    if (!m_idToWd.contains(id)) {
      m_followed.erase(id);
      return -1;
    }
    return id;
  }
  const int wd = inotify_add_watch(m_fd, dir.c_str(), kMask);
  if (wd < 0) {
    kLog.warn("cannot watch {}: {}", dir.string(), std::strerror(errno));
    return -1;
  }
  subscribe(id, wd, std::move(callback));
  return id;
}

void DirectoryWatcher::subscribe(WatchId id, int wd, Callback callback) {
  // inotify returns the same wd for the same directory: subscribers share it.
  m_watches[wd].subscribers.emplace_back(id, std::move(callback));
  m_idToWd[id] = wd;
}

void DirectoryWatcher::unwatch(WatchId id) {
  m_followed.erase(id);
  detach(id);
}

void DirectoryWatcher::detach(WatchId id) {
  const auto it = m_idToWd.find(id);
  if (it == m_idToWd.end()) {
    return;
  }
  const int wd = it->second;
  m_idToWd.erase(it);
  auto watch = m_watches.find(wd);
  if (watch == m_watches.end()) {
    return;
  }
  std::erase_if(watch->second.subscribers, [id](const auto& s) { return s.first == id; });
  if (watch->second.subscribers.empty()) {
    m_watches.erase(watch);
    inotify_rm_watch(m_fd, wd);
    std::erase_if(m_pendingMoves, [wd](const PendingMove& m) { return m.wd == wd; });
  }
}

void DirectoryWatcher::follow(WatchId id, bool announce) {
  detach(id);
  const auto followed = m_followed.find(id);
  if (followed == m_followed.end()) {
    return;
  }
  const std::filesystem::path target = followed->second.path;
  for (;;) {
    std::error_code ec;
    std::filesystem::path dir = target;
    while (!std::filesystem::is_directory(dir, ec) && dir.has_relative_path()) {
      dir = dir.parent_path();
    }
    int wd = inotify_add_watch(m_fd, dir.c_str(), dir == target ? kMask : kAncestorMask);
    int error = wd < 0 ? errno : 0;
    // A directory that exists but cannot be watched (permission, watch limit): wait on its parent, whose events
    // (a chmod, a recreate) try it again.
    bool blocked = false;
    while (wd < 0 && error != ENOENT && error != ENOTDIR && dir.has_relative_path()) {
      kLog.warn("cannot watch {}: {}; waiting on its parent", dir.string(), std::strerror(error));
      dir = dir.parent_path();
      wd = inotify_add_watch(m_fd, dir.c_str(), kAncestorMask | IN_ATTRIB);
      error = wd < 0 ? errno : 0;
      blocked = true;
    }
    if (wd < 0) {
      if (error == ENOENT || error == ENOTDIR) {
        continue; // removed between the check and the watch
      }
      kLog.warn("cannot watch {}: {}", dir.string(), std::strerror(error));
      return;
    }
    if (dir == target) {
      subscribe(id, wd, [this, id, wd](Event event, const std::string& name, const std::string& newName) {
        const auto it = m_followed.find(id);
        if (it == m_followed.end()) {
          return;
        }
        const Callback callback = it->second.callback;
        if (event != Event::Gone) {
          callback(event, name, newName);
          return;
        }
        // Moves out still waiting for their pair end with the directory: deliver them before it goes.
        std::vector<std::string> movedOut;
        for (const PendingMove& move : m_pendingMoves) {
          if (move.wd == wd) {
            movedOut.push_back(move.name);
          }
        }
        for (const std::string& moved : movedOut) {
          callback(Event::Deleted, moved, {});
        }
        callback(Event::Gone, {}, {});
        if (m_followed.contains(id)) {
          follow(id, true);
        }
      });
      if (announce) {
        const Callback callback = followed->second.callback;
        callback(Event::Overflow, {}, {});
      }
      return;
    }
    const std::string next = target.lexically_relative(dir).begin()->string();
    subscribe(id, wd, [this, id, next](Event event, const std::string& name, const std::string& newName) {
      bool arrived = false;
      switch (event) {
      case Event::Created:
      case Event::MovedIn:
      case Event::Changed: // a chmod may make a blocked directory watchable
        arrived = name == next;
        break;
      case Event::Renamed:
        arrived = newName == next;
        break;
      case Event::Gone:
      case Event::Overflow:
        arrived = true;
        break;
      case Event::Deleted:
      case Event::Written:
        break;
      }
      if (arrived) {
        follow(id, true);
      }
    });
    // The next component may have appeared before the watch: no event will come for it.
    if (blocked || !std::filesystem::is_directory(dir / next, ec)) {
      return;
    }
    detach(id);
  }
}

int DirectoryWatcher::pollTimeoutMs() const {
  if (m_pendingMoves.empty()) {
    return -1;
  }
  const auto left = std::chrono::ceil<std::chrono::milliseconds>(
      m_pendingMoves.front().deadline - std::chrono::steady_clock::now()
  );
  return static_cast<int>(std::max<std::int64_t>(0, left.count()));
}

void DirectoryWatcher::doAddPollFds(std::vector<pollfd>& fds) {
  if (m_fd >= 0) {
    fds.push_back({.fd = m_fd, .events = POLLIN, .revents = 0});
  }
}

void DirectoryWatcher::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  const bool readable = startIdx < fds.size() && fds[startIdx].fd == m_fd && fds[startIdx].revents != 0;
  if (readable) {
    processEvents();
  }
  flushPendingMoves();
}

void DirectoryWatcher::processEvents() {
  if (m_fd < 0) {
    return;
  }
  // Copies: a callback may unwatch or watch while events for the same wd are still queued.
  const auto deliver = [this](int wd, Event event, const std::string& name, const std::string& newName = {}) {
    const auto it = m_watches.find(wd);
    if (it == m_watches.end()) {
      return;
    }
    const auto subscribers = it->second.subscribers;
    for (const auto& [id, callback] : subscribers) {
      if (subscribed(id, wd) && callback) {
        callback(event, name, newName);
      }
    }
  };

  alignas(inotify_event) std::array<char, 16384> buffer{};
  for (;;) {
    const ssize_t n = ::read(m_fd, buffer.data(), buffer.size());
    if (n <= 0) {
      if (n < 0 && errno == EINTR) {
        continue;
      }
      break;
    }
    for (ssize_t off = 0; off < n;) {
      const auto* ev = reinterpret_cast<const inotify_event*>(buffer.data() + off);
      off += static_cast<ssize_t>(sizeof(inotify_event) + ev->len);
      const std::string name = ev->len > 0 ? std::string(ev->name) : std::string{};
      if ((ev->mask & IN_Q_OVERFLOW) != 0) {
        m_pendingMoves.clear();
        std::vector<int> wds;
        for (const auto& [wd, _] : m_watches) {
          wds.push_back(wd);
        }
        for (const int wd : wds) {
          deliver(wd, Event::Overflow, {});
        }
        continue;
      }
      if ((ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) != 0) {
        deliver(ev->wd, Event::Gone, {});
        continue;
      }
      if ((ev->mask & IN_IGNORED) != 0) {
        continue;
      }
      // A later event for a name that moved out: the move-out happened first, deliver it first.
      if (const auto out = std::ranges::find_if(m_pendingMoves, [&](const PendingMove& m) {
            return m.wd == ev->wd && m.name == name && (m.cookie != ev->cookie || (ev->mask & IN_MOVED_TO) == 0);
          });
          !name.empty() && out != m_pendingMoves.end()) {
        const PendingMove move = *out;
        m_pendingMoves.erase(out);
        deliver(move.wd, Event::Deleted, move.name);
      }
      if ((ev->mask & IN_MOVED_FROM) != 0) {
        m_pendingMoves.push_back({
            .wd = ev->wd,
            .cookie = ev->cookie,
            .name = name,
            .deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kMovePairMs),
        });
        continue;
      }
      if ((ev->mask & IN_MOVED_TO) != 0) {
        const auto from = std::ranges::find_if(m_pendingMoves, [&](const PendingMove& m) { return m.cookie == ev->cookie; });
        if (from == m_pendingMoves.end()) {
          deliver(ev->wd, Event::MovedIn, name);
          continue;
        }
        const PendingMove move = *from;
        m_pendingMoves.erase(from);
        if (move.wd == ev->wd) {
          deliver(ev->wd, Event::Renamed, move.name, name);
        } else {
          deliver(move.wd, Event::Deleted, move.name);
          deliver(ev->wd, Event::MovedIn, name);
        }
        continue;
      }
      if ((ev->mask & IN_CREATE) != 0) {
        deliver(ev->wd, Event::Created, name);
      } else if ((ev->mask & IN_DELETE) != 0) {
        deliver(ev->wd, Event::Deleted, name);
      } else if ((ev->mask & IN_CLOSE_WRITE) != 0) {
        deliver(ev->wd, Event::Written, name);
      } else if ((ev->mask & (IN_MODIFY | IN_ATTRIB)) != 0) {
        deliver(ev->wd, Event::Changed, name);
      }
    }
  }
  flushPendingMoves();
}

void DirectoryWatcher::flushPendingMoves() {
  const auto now = std::chrono::steady_clock::now();
  std::vector<PendingMove> due;
  std::erase_if(m_pendingMoves, [&](const PendingMove& m) {
    if (now >= m.deadline) {
      due.push_back(m);
      return true;
    }
    return false;
  });
  for (const PendingMove& move : due) {
    const auto it = m_watches.find(move.wd);
    if (it == m_watches.end()) {
      continue;
    }
    const auto subscribers = it->second.subscribers;
    for (const auto& [id, callback] : subscribers) {
      if (subscribed(id, move.wd) && callback) {
        callback(Event::Deleted, move.name, {});
      }
    }
  }
}
