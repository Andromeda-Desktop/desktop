#pragma once

#include "core/poll_source.h"

#include <functional>

class PollLoop;

// One fd on a PollLoop: `callback` gets the fd's poll(2) revents when any is set. Joins the loop on construction and
// leaves it on destruction; the callback may destroy the watch.
class FdWatch final : public PollSource {
public:
  FdWatch(PollLoop& loop, int fd, short events, std::function<void(short revents)> callback);
  ~FdWatch() override;
  FdWatch(const FdWatch&) = delete;
  FdWatch& operator=(const FdWatch&) = delete;

  // 0 keeps the fd out of the poll set (no POLLERR/POLLHUP either).
  void setEvents(short events) { m_events = events; }
  [[nodiscard]] int fd() const noexcept { return m_fd; }

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

private:
  void doAddPollFds(std::vector<pollfd>& fds) override;

  PollLoop& m_loop;
  int m_fd;
  short m_events;
  std::function<void(short)> m_callback;
};
