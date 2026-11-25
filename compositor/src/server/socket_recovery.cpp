#include "server/socket_recovery.h"
#include "core/log.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-server-core.h>

namespace andromeda {

  namespace {
    constexpr Logger kLog("socket");
  } // namespace

  SocketRecovery::SocketRecovery(wl_event_loop* loop, std::string runtimeDir) : m_runtimeDir(std::move(runtimeDir)) {
    m_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m_fd < 0 || inotify_add_watch(m_fd, m_runtimeDir.c_str(), IN_DELETE | IN_MOVED_FROM | IN_DELETE_SELF) < 0) {
      kLog.warn("inotify on {} unavailable: a deleted socket stays deleted ({})", m_runtimeDir, std::strerror(errno));
      stop();
      return;
    }
    m_source = wl_event_loop_add_fd(loop, m_fd, WL_EVENT_READABLE, onReadable, this);
    if (m_source == nullptr) {
      kLog.warn("socket watch: no event source; a deleted socket stays deleted");
      stop();
    }
  }

  SocketRecovery::~SocketRecovery() { stop(); }

  void SocketRecovery::stop() {
    if (m_source != nullptr) {
      wl_event_source_remove(m_source);
      m_source = nullptr;
    }
    if (m_fd >= 0) {
      close(m_fd);
      m_fd = -1;
    }
  }

  void SocketRecovery::watch(const std::string& name, std::function<bool()> recover) {
    if (m_fd >= 0) {
      m_entries[name] = Entry{.recover = std::move(recover)};
    }
  }

  void SocketRecovery::handle(const std::string& name) {
    const auto it = m_entries.find(name);
    if (it == m_entries.end()) {
      return;
    }
    const std::string path = m_runtimeDir + "/" + name;
    struct stat st {};
    if (lstat(path.c_str(), &st) == 0) {
      return; // replaced by a rename or another process: not ours to touch
    }
    if (++it->second.recoveries > kMaxRecoveries) {
      kLog.error("{} deleted again: gave up after {} recoveries", path, kMaxRecoveries);
      m_entries.erase(it);
      return;
    }
    if (it->second.recover()) {
      kLog.warn("{} was deleted: recreated ({}/{})", path, it->second.recoveries, kMaxRecoveries);
    } else {
      kLog.error("{} was deleted and could not be recreated; new clients cannot connect", path);
      m_entries.erase(it);
    }
  }

  int SocketRecovery::onReadable(int fd, uint32_t /*mask*/, void* data) {
    auto* self = static_cast<SocketRecovery*>(data);
    alignas(inotify_event) std::array<char, 4096> buffer{};
    bool overflow = false;
    bool dirGone = false;
    std::vector<std::string> deleted;
    for (;;) {
      const ssize_t n = read(fd, buffer.data(), buffer.size());
      if (n <= 0) {
        break;
      }
      for (ssize_t offset = 0; offset < n;) {
        const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
        overflow |= (event->mask & IN_Q_OVERFLOW) != 0;
        dirGone |= (event->mask & (IN_DELETE_SELF | IN_IGNORED)) != 0;
        if (event->len > 0 && (event->mask & (IN_DELETE | IN_MOVED_FROM)) != 0) {
          deleted.emplace_back(event->name);
        }
        offset += static_cast<ssize_t>(sizeof(inotify_event) + event->len);
      }
    }
    if (dirGone) {
      kLog.error("{} was removed: sockets cannot be recreated", self->m_runtimeDir);
      self->stop();
      return 0;
    }
    if (overflow) {
      for (const auto& [name, entry] : self->m_entries) {
        deleted.push_back(name);
      }
    }
    for (const std::string& name : deleted) {
      self->handle(name);
    }
    return 0;
  }

} // namespace andromeda
