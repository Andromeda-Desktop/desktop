#include "workspace_watcher.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace term {

  WorkspaceWatcher::WorkspaceWatcher(std::function<void()> changed, std::function<Defs()> load,
                                     std::function<void(const Defs&)> save)
      : m_changed(std::move(changed)), m_load(std::move(load)), m_save(std::move(save)) {}

  WorkspaceWatcher::~WorkspaceWatcher() { disconnect(); }

  bool WorkspaceWatcher::connect() {
    if (m_fd >= 0) return true;
    // A unix connect to the listening (socket-activated) holder returns at once.
    m_fd = holder::connectSocket();
    if (m_fd < 0) return false;
    ::fcntl(m_fd, F_SETFL, ::fcntl(m_fd, F_GETFL) | O_NONBLOCK);
    m_sync = AwaitTable{};
    send(holder::encodeWatch());
    return m_fd >= 0;
  }

  std::optional<std::uint32_t> WorkspaceWatcher::put(const holder::WorkspaceInfo& def,
                                                     std::function<void(std::uint32_t, std::uint64_t)> done) {
    if (!connect()) return std::nullopt;
    const std::uint32_t req = m_nextReq++;
    m_puts.emplace(req, std::move(done));
    send(holder::encodePutWorkspace(req, def.id, def.name, def.root, def.symbol));
    if (m_fd < 0) return std::nullopt;
    return req;
  }

  void WorkspaceWatcher::setSession(std::uint64_t heldId, std::uint64_t workspace) {
    if (connect()) send(holder::encodeSetWorkspace(heldId, workspace));
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
    std::vector<holder::Message> msgs;
    std::size_t used = 0;
    for (;;) {
      std::optional<holder::Message> msg;
      const auto took = holder::decode(std::span(m_in).subspan(used), msg);
      if (!took) broken = true;
      if (!took || *took == 0) break;
      used += *took;
      msgs.push_back(std::move(*msg));
    }
    m_in.erase(m_in.begin(), m_in.begin() + static_cast<std::ptrdiff_t>(used));
    // After the buffer work: handling may send, and a failed send closes the connection.
    const int fd = m_fd;
    bool changed = false;
    for (holder::Message& msg : msgs) {
      if (m_fd != fd) break;
      if (auto* table = std::get_if<holder::Workspaces>(&msg); table != nullptr && (!m_seq || table->seq > *m_seq)) {
        m_seq = table->seq;
        m_table = std::move(*table);
        changed = true;
        syncDefs();
      } else if (auto* answer = std::get_if<holder::WorkspacePut>(&msg)) {
        if (auto node = m_puts.extract(answer->req)) node.mapped()(answer->req, answer->id);
      }
    }
    if (broken || m_fd != fd) {
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
    m_puts.clear();
    m_sync = AwaitTable{};
  }

  void WorkspaceWatcher::syncDefs() {
    Defs defs;
    for (const holder::WorkspaceInfo& w : m_table.workspaces) defs.push_back({.name = w.name, .root = w.root, .symbol = w.symbol});
    if (std::holds_alternative<AwaitTable>(m_sync)) {
      m_saved = m_load();
      Defs held = defs;
      Defs missing;
      for (const holder::WorkspaceInfo& def : m_saved) {
        if (const auto it = std::ranges::find(held, def); it != held.end()) {
          held.erase(it);
        } else {
          missing.push_back(def);
        }
      }
      m_sync = missing.empty() ? decltype(m_sync){Live{}} : decltype(m_sync){Restoring{}};
      const int fd = m_fd;
      for (const holder::WorkspaceInfo& def : missing) {
        const auto req = put(def, [this](std::uint32_t answered, std::uint64_t) {
          auto* restoring = std::get_if<Restoring>(&m_sync);
          if (restoring != nullptr && restoring->reqs.erase(answered) == 1 && restoring->reqs.empty()) m_sync = Live{};
        });
        if (!req || m_fd != fd) return;
        std::get<Restoring>(m_sync).reqs.insert(*req);
      }
    }
    if (std::holds_alternative<Live>(m_sync) && defs != m_saved) {
      m_saved = defs;
      m_save(m_saved);
    }
  }

} // namespace term
