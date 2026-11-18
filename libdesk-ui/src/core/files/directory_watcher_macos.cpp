#include "core/files/directory_watcher.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/event.h>
#include <unistd.h>

// kqueue reports that a directory changed, not which entry: every change arrives as Overflow, so the caller rescans.
namespace {
  constexpr Logger kLog("dir-watch");
  constexpr unsigned kFlags = NOTE_WRITE | NOTE_ATTRIB | NOTE_EXTEND | NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE;
} // namespace

DirectoryWatcher::DirectoryWatcher() : m_fd(kqueue()) {
  if (m_fd < 0) {
    kLog.warn("kqueue unavailable: {}", std::strerror(errno));
    return;
  }
  ::fcntl(m_fd, F_SETFD, FD_CLOEXEC);
}

DirectoryWatcher::~DirectoryWatcher() {
  for (const auto& [wd, watch] : m_watches) {
    ::close(wd);
  }
  if (m_fd >= 0) {
    ::close(m_fd);
  }
}

DirectoryWatcher::WatchId DirectoryWatcher::watch(const std::filesystem::path& dir, Callback callback) {
  if (m_fd < 0) {
    return -1;
  }
  // The watch descriptor is the directory's own event-only fd.
  const int wd = ::open(dir.c_str(), O_EVTONLY | O_DIRECTORY | O_CLOEXEC);
  if (wd < 0) {
    kLog.warn("cannot watch {}: {}", dir.string(), std::strerror(errno));
    return -1;
  }
  struct kevent change{};
  EV_SET(&change, wd, EVFILT_VNODE, EV_ADD | EV_CLEAR, kFlags, 0, nullptr);
  if (::kevent(m_fd, &change, 1, nullptr, 0, nullptr) != 0) {
    kLog.warn("cannot watch {}: {}", dir.string(), std::strerror(errno));
    ::close(wd);
    return -1;
  }
  const int id = m_nextId++;
  m_watches[wd].subscribers.emplace_back(id, std::move(callback));
  m_idToWd[id] = wd;
  return id;
}

void DirectoryWatcher::unwatch(WatchId id) {
  const auto it = m_idToWd.find(id);
  if (it == m_idToWd.end()) {
    return;
  }
  const int wd = it->second;
  m_idToWd.erase(it);
  m_watches.erase(wd);
  // Closing the fd also removes its kevent.
  ::close(wd);
}

int DirectoryWatcher::pollTimeoutMs() const { return -1; }

void DirectoryWatcher::doAddPollFds(std::vector<pollfd>& fds) {
  if (m_fd >= 0) {
    fds.push_back({.fd = m_fd, .events = POLLIN, .revents = 0});
  }
}

void DirectoryWatcher::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (startIdx < fds.size() && fds[startIdx].fd == m_fd && fds[startIdx].revents != 0) {
    processEvents();
  }
}

void DirectoryWatcher::processEvents() {
  if (m_fd < 0) {
    return;
  }
  std::array<struct kevent, 64> events{};
  const timespec now{};
  for (;;) {
    const int n = ::kevent(m_fd, nullptr, 0, events.data(), static_cast<int>(events.size()), &now);
    if (n <= 0) {
      return;
    }
    for (int i = 0; i < n; ++i) {
      const int wd = static_cast<int>(events[static_cast<std::size_t>(i)].ident);
      const auto flags = events[static_cast<std::size_t>(i)].fflags;
      const Event event = (flags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE)) != 0 ? Event::Gone : Event::Overflow;
      const auto watch = m_watches.find(wd);
      if (watch == m_watches.end()) {
        continue;
      }
      // A copy: a callback may unwatch or watch.
      const auto subscribers = watch->second.subscribers;
      for (const auto& [id, callback] : subscribers) {
        if (m_idToWd.contains(id) && callback) {
          callback(event, {}, {});
        }
      }
    }
  }
}

void DirectoryWatcher::flushPendingMoves() {}
