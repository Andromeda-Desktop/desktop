#pragma once

// The wait set of a poll loop. POSIX: poll(2) on fds. Windows: fd holds a kernel object HANDLE (HandleToLong:
// 64-bit Windows handles fit in 32 bits) and POLLIN waits until it is signaled; POLLOUT means nothing.
#ifdef _WIN32
#include <cstddef>

struct pollfd {
  int fd;
  short events;
  short revents;
};
using nfds_t = std::size_t;
#define POLLIN 0x0001
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020
#else
#include <poll.h>
#endif

namespace sys {

  // poll(2) semantics on every OS. Windows: a handle signaled during the wait reports POLLIN; at most
  // MAXIMUM_WAIT_OBJECTS distinct handles (more: -1 and EINVAL). A handle must stay signaled until its source
  // dispatches (a manual-reset event): the check of the other handles consumes an auto-reset one.
  int pollFds(pollfd* fds, nfds_t count, int timeoutMs);

} // namespace sys
