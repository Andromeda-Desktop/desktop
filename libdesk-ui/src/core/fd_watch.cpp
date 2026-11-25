#include "core/fd_watch.h"

#include "core/poll_loop.h"

FdWatch::FdWatch(PollLoop& loop, int fd, short events, std::function<void(short revents)> callback)
    : m_loop(loop), m_fd(fd), m_events(events), m_callback(std::move(callback)) {
  m_loop.addSource(this);
}

FdWatch::~FdWatch() {
  m_loop.removeSource(this);
}

void FdWatch::doAddPollFds(std::vector<pollfd>& fds) {
  fds.push_back({.fd = m_events != 0 ? m_fd : -1, .events = m_events, .revents = 0});
}

void FdWatch::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (startIdx >= fds.size() || fds[startIdx].revents == 0) {
    return;
  }
  // Held outside the watch while it runs: the callback may destroy the watch.
  const std::weak_ptr<const void> alive = liveness();
  std::function<void(short)> callback = std::move(m_callback);
  callback(fds[startIdx].revents);
  if (!alive.expired()) {
    m_callback = std::move(callback);
  }
}
