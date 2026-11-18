#include "workspace_watcher.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace term {

  WorkspaceWatcher::WorkspaceWatcher(std::function<void()> changed) : m_changed(std::move(changed)) {}

  WorkspaceWatcher::~WorkspaceWatcher() { disconnect(); }

  bool WorkspaceWatcher::connect() {
    if (m_fd >= 0) return true;
    // A unix connect to the listening (socket-activated) holder returns at once.
    m_fd = holder::connectSocket();
    if (m_fd < 0) return false;
    ::fcntl(m_fd, F_SETFL, ::fcntl(m_fd, F_GETFL) | O_NONBLOCK);
    send(holder::encodeWatch());
    return m_fd >= 0;
  }

  void WorkspaceWatcher::doAddPollFds(std::vector<pollfd>& fds) {
    if (m_fd < 0) return;
    fds.push_back({.fd = m_fd, .events = static_cast<short>(POLLIN | (m_out.empty() ? 0 : POLLOUT)), .revents = 0});
  }

  void WorkspaceWatcher::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
    if (m_fd < 0 || startIdx >= fds.size() || fds[startIdx].fd != m_fd) return;
    const short revents = fds[startIdx].revents;
    if ((revents & POLLOUT) != 0) flush();
    if (m_fd >= 0 && (revents & (POLLIN | POLLHUP | POLLERR)) != 0) read();
  }

  void WorkspaceWatcher::send(std::vector<std::uint8_t> frame) {
    m_out.insert(m_out.end(), frame.begin(), frame.end());
    flush();
  }

  void WorkspaceWatcher::flush() {
    while (!m_out.empty()) {
      const ssize_t n = ::send(m_fd, m_out.data(), m_out.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
      if (n > 0) {
        m_out.erase(m_out.begin(), m_out.begin() + n);
      } else if (n < 0 && errno == EINTR) {
        continue;
      } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
      } else {
        disconnect();
        return;
      }
    }
  }

  void WorkspaceWatcher::read() {
    std::vector<int> fds;
    long n;
    while ((n = holder::receive(m_fd, m_in, fds, true)) > 0) {}
    for (const int fd : fds) ::close(fd);
    bool broken = n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
    bool changed = false;
    std::size_t used = 0;
    for (;;) {
      std::optional<holder::Message> msg;
      const auto took = holder::decode(std::span(m_in).subspan(used), msg);
      if (!took) broken = true;
      if (!took || *took == 0) break;
      used += *took;
      if (auto* table = std::get_if<holder::Workspaces>(&*msg); table != nullptr && (!m_seq || table->seq > *m_seq)) {
        m_seq = table->seq;
        m_table = std::move(*table);
        changed = true;
      }
    }
    m_in.erase(m_in.begin(), m_in.begin() + static_cast<std::ptrdiff_t>(used));
    if (broken) {
      disconnect();
      // A restarted holder starts a new table; its socket unit takes the connection at once.
      connect();
    }
    if (changed) m_changed();
  }

  void WorkspaceWatcher::disconnect() {
    if (m_fd >= 0) ::close(m_fd);
    m_fd = -1;
    m_in.clear();
    m_out.clear();
    m_seq.reset();
  }

} // namespace term
