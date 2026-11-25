#include "ipc_channel.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>

#ifndef SO_PEERPIDFD
#define SO_PEERPIDFD 77
#endif

namespace ipc {

  namespace {
    // Milliseconds left until the deadline for poll(), at least 0.
    int remainingMs(Deadline deadline) {
      const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      return static_cast<int>(std::clamp<std::chrono::milliseconds::rep>(left.count(), 0, 1'000'000));
    }

    // True when fd is ready for events before the deadline.
    bool waitFor(int fd, short events, Deadline deadline) {
      for (;;) {
        pollfd p{.fd = fd, .events = events, .revents = 0};
        const int ready = poll(&p, 1, deadline == Deadline::max() ? -1 : remainingMs(deadline));
        if (ready > 0) {
          return true;
        }
        if (ready == 0) {
          errno = ETIMEDOUT;
          return false;
        }
        if (errno != EINTR) {
          return false;
        }
      }
    }
  } // namespace

  Fd& Fd::operator=(Fd&& other) noexcept {
    if (this != &other) {
      if (m_fd >= 0) {
        close(m_fd);
      }
      m_fd = other.release();
    }
    return *this;
  }

  Fd::~Fd() {
    if (m_fd >= 0) {
      close(m_fd);
    }
  }

  int Fd::release() {
    const int fd = m_fd;
    m_fd = -1;
    return fd;
  }

  bool running(int pidfd) {
    // poll() skips a negative fd and reports nothing, which would read as "still runs".
    if (pidfd < 0) {
      return false;
    }
    // A pidfd becomes readable when its process exits; a closed fd gives POLLNVAL, also "not running".
    pollfd exited{.fd = pidfd, .events = POLLIN, .revents = 0};
    int ready = 0;
    do {
      ready = poll(&exited, 1, 0);
    } while (ready < 0 && errno == EINTR);
    return ready == 0;
  }

  std::optional<Peer> checkPeer(Fd conn) {
    ucred cred{};
    socklen_t credSize = sizeof(cred);
    int pidfd = -1;
    socklen_t pidfdSize = sizeof(pidfd);
    if (getsockopt(conn.get(), SOL_SOCKET, SO_PEERCRED, &cred, &credSize) != 0 || cred.uid != geteuid()
        || getsockopt(conn.get(), SOL_SOCKET, SO_PEERPIDFD, &pidfd, &pidfdSize) != 0 || pidfd < 0) {
      errno = EACCES;
      return std::nullopt;
    }
    return Peer{.conn = std::move(conn), .pid = cred.pid, .gid = cred.gid, .pidfd = Fd(pidfd)};
  }

  std::optional<Peer> acceptPeer(int listenFd, int flags) {
    Fd conn(accept4(listenFd, nullptr, nullptr, flags | SOCK_CLOEXEC));
    if (!conn) {
      return std::nullopt;
    }
    return checkPeer(std::move(conn));
  }

  std::size_t LineReader::readFrom(int fd) {
    if (m_state == State::TooLong && m_dropped < m_cap) {
      char chunk[8192];
      const ssize_t size = read(fd, chunk, std::min(sizeof(chunk), m_cap - m_dropped));
      const std::size_t dropped = size > 0 ? static_cast<std::size_t>(size) : 0;
      m_dropped += dropped;
      return dropped;
    }
    if (m_state != State::Reading) {
      return 0;
    }
    if (m_start > 0) {
      m_buffer.erase(0, m_start);
      m_start = 0;
    }
    const std::size_t room = m_cap - std::min(m_buffer.size(), m_cap);
    if (room == 0) {
      // A complete line waits for nextLine(); else the line does not fit.
      if (m_buffer.find('\n') == std::string::npos) {
        m_buffer.clear();
        m_state = State::TooLong;
      }
      return 0;
    }
    char chunk[8192];
    ssize_t size = -1;
    do {
      size = read(fd, chunk, std::min(sizeof(chunk), room));
    } while (size < 0 && errno == EINTR);
    if (size > 0) {
      m_buffer.append(chunk, static_cast<std::size_t>(size));
    }
    if (size == 0 || (size < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
      m_state = State::Closed;
    } else if (m_buffer.size() >= m_cap && m_buffer.find('\n') == std::string::npos) {
      m_buffer.clear();
      m_state = State::TooLong;
    }
    return size > 0 ? static_cast<std::size_t>(size) : 0;
  }

  std::optional<std::string> LineReader::nextLine() {
    const auto end = m_buffer.find('\n', m_start);
    if (end == std::string::npos) {
      return std::nullopt;
    }
    std::string line = m_buffer.substr(m_start, end - m_start);
    m_start = end + 1;
    if (m_start == m_buffer.size()) {
      m_buffer.clear();
      m_start = 0;
    }
    return line;
  }

  std::string LineReader::takeRest() {
    std::string rest = m_buffer.substr(m_start);
    m_buffer.clear();
    m_start = 0;
    return rest;
  }

  ClientSlots::Slot& ClientSlots::Slot::operator=(Slot&& other) noexcept {
    if (this != &other) {
      if (m_owner != nullptr) {
        --m_owner->m_used;
      }
      m_owner = other.m_owner;
      other.m_owner = nullptr;
    }
    return *this;
  }

  ClientSlots::Slot::~Slot() {
    if (m_owner != nullptr) {
      --m_owner->m_used;
    }
  }

  std::optional<ClientSlots::Slot> ClientSlots::acquire() {
    if (m_used >= m_max) {
      return std::nullopt;
    }
    ++m_used;
    return Slot(this);
  }

  bool writeAll(int fd, std::string_view bytes, Deadline deadline) {
    while (!bytes.empty()) {
      const ssize_t sent = send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
      if (sent > 0) {
        bytes.remove_prefix(static_cast<std::size_t>(sent));
      } else if (sent < 0 && errno == EINTR) {
        continue;
      } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (!waitFor(fd, POLLOUT, deadline)) {
          return false;
        }
      } else {
        return false;
      }
    }
    return true;
  }

  Read readAll(int fd, std::size_t cap, Deadline deadline, bool (*complete)(std::string_view)) {
    Read result;
    char chunk[8192];
    for (;;) {
      if (complete != nullptr && !result.data.empty() && complete(result.data)) {
        result.status = Status::Ok;
        return result;
      }
      const std::size_t room = cap - result.data.size();
      const ssize_t size = recv(fd, chunk, std::min(sizeof(chunk), room + 1), MSG_DONTWAIT);
      if (size == 0) {
        result.status = Status::Ok;
        return result;
      }
      if (size > 0) {
        const auto got = static_cast<std::size_t>(size);
        result.data.append(chunk, std::min(got, room));
        if (got > room) {
          result.status = Status::TooLong;
          return result;
        }
        continue;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        result.status = Status::Failed;
        return result;
      }
      if (!waitFor(fd, POLLIN, deadline)) {
        result.status = Status::TimedOut;
        return result;
      }
    }
  }

  Fd connectTo(const std::string& path, Deadline deadline, int type) {
    sockaddr_un addr{};
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
      errno = path.empty() ? EINVAL : ENAMETOOLONG;
      return {};
    }
    const int ms = remainingMs(deadline);
    const bool nonBlocking = (type & SOCK_NONBLOCK) != 0;
    // A non-blocking AF_UNIX connect fails with EAGAIN on a full backlog instead of waiting.
    Fd fd(socket(AF_UNIX, (ms == 0 ? type | SOCK_NONBLOCK : type & ~SOCK_NONBLOCK) | SOCK_CLOEXEC, 0));
    if (!fd) {
      return {};
    }
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size());
    if (ms > 0) {
      // A blocking connect waits for backlog room for at most SO_SNDTIMEO.
      const timeval timeout{.tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000};
      setsockopt(fd.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    }
    int rc = -1;
    do {
      rc = connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    } while (rc != 0 && errno == EINTR && remainingMs(deadline) > 0);
    const int flags = fcntl(fd.get(), F_GETFL);
    if (rc != 0 || flags < 0
        || fcntl(fd.get(), F_SETFL, nonBlocking ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) != 0) {
      const int error = errno;
      fd = Fd();
      errno = error;
      return {};
    }
    return fd;
  }

  bool unixPathListens(const std::string& path) {
    // EPROTOTYPE (a live socket of another type) counts as listening.
    return static_cast<bool>(connectTo(path, Deadline{})) || (errno != ECONNREFUSED && errno != ENOENT);
  }

  Fd listenUnix(const std::string& path, int type, int backlog, mode_t mode, Stale stale) {
    sockaddr_un addr{};
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
      errno = path.empty() ? EINVAL : ENAMETOOLONG;
      return {};
    }
    Fd fd(socket(AF_UNIX, type | SOCK_CLOEXEC, 0));
    if (!fd) {
      return {};
    }
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size());
    // Held until listen: a server that probes while another binds would unlink that server's fresh socket.
    Fd dirLock;
    if (stale == Stale::Replace) {
      const std::size_t slash = path.rfind('/');
      const std::string dir = slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
      dirLock = Fd(open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
      int rc = -1;
      while (dirLock && (rc = flock(dirLock.get(), LOCK_EX)) != 0 && errno == EINTR) {
      }
      if (rc != 0) {
        return {};
      }
      if (unixPathListens(path)) {
        errno = EADDRINUSE;
        return {};
      }
      unlink(path.c_str());
    }
    int error = 0;
    // Linux creates the socket file with the socket inode's mode minus the umask, so the path never has wider access.
    if (mode != 0 && fchmod(fd.get(), mode) != 0) {
      error = errno;
    } else if (bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
      error = errno;
    } else if ((mode != 0 && chmod(path.c_str(), mode) != 0) || listen(fd.get(), backlog) != 0) {
      error = errno;
      unlink(path.c_str());
    }
    if (error != 0) {
      fd = Fd();
      errno = error;
      return {};
    }
    return fd;
  }

  ssize_t sendFds(int fd, std::string_view bytes, std::span<const int> fds, Deadline deadline) {
    iovec iov{.iov_base = const_cast<char*>(bytes.data()), .iov_len = bytes.size()};
    msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    if (fds.size() > kMaxFds) {
      errno = EINVAL;
      return -1;
    }
    alignas(cmsghdr) std::array<char, CMSG_SPACE(kMaxFds * sizeof(int))> control{};
    if (!fds.empty()) {
      message.msg_control = control.data();
      message.msg_controllen = CMSG_SPACE(fds.size_bytes());
      cmsghdr* header = CMSG_FIRSTHDR(&message);
      header->cmsg_level = SOL_SOCKET;
      header->cmsg_type = SCM_RIGHTS;
      header->cmsg_len = CMSG_LEN(fds.size_bytes());
      std::memcpy(CMSG_DATA(header), fds.data(), fds.size_bytes());
    }
    for (;;) {
      const ssize_t sent = sendmsg(fd, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
      if (sent >= 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
        return sent;
      }
      if (errno != EINTR && (remainingMs(deadline) == 0 || !waitFor(fd, POLLOUT, deadline))) {
        return -1;
      }
    }
  }

  Received recvFds(int fd, std::span<std::byte> buffer, std::size_t maxFds) {
    iovec iov{.iov_base = buffer.data(), .iov_len = buffer.size()};
    maxFds = std::min(maxFds, kMaxFds);
    alignas(cmsghdr) std::array<char, CMSG_SPACE(kMaxFds * sizeof(int))> control{};
    msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = maxFds == 0 ? nullptr : control.data();
    message.msg_controllen = maxFds == 0 ? 0 : CMSG_SPACE(maxFds * sizeof(int));
    ssize_t count = -1;
    do {
      count = recvmsg(fd, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    } while (count < 0 && errno == EINTR);
    Received result;
    if (count < 0) {
      result.kind = errno == EAGAIN || errno == EWOULDBLOCK ? Received::Kind::WouldBlock : Received::Kind::Failed;
      return result;
    }
    bool truncated = (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0;
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr; header = CMSG_NXTHDR(&message, header)) {
      if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS || header->cmsg_len < CMSG_LEN(0)) {
        continue;
      }
      const std::size_t n = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      for (std::size_t i = 0; i < n; ++i) {
        int received = -1;
        std::memcpy(&received, CMSG_DATA(header) + i * sizeof(int), sizeof(int));
        result.fds.emplace_back(received);
      }
    }
    // CMSG_SPACE rounds up, so the kernel can deliver more fds than asked for.
    truncated = truncated || result.fds.size() > maxFds;
    result.size = static_cast<std::size_t>(count);
    result.kind = truncated ? Received::Kind::Truncated : count == 0 ? Received::Kind::Closed : Received::Kind::Data;
    return result;
  }

  Read request(const std::string& path, std::string_view bytes, EndOfRequest end, std::size_t cap, Deadline deadline,
               bool (*complete)(std::string_view)) {
    const Fd fd = connectTo(path, deadline);
    if (!fd) {
      return {.status = Status::NoServer, .data = {}};
    }
    if (!writeAll(fd.get(), bytes, deadline)) {
      return {.status = remainingMs(deadline) == 0 ? Status::TimedOut : Status::Failed, .data = {}};
    }
    if (end == EndOfRequest::HalfClose) {
      shutdown(fd.get(), SHUT_WR);
    }
    return readAll(fd.get(), cap, deadline, complete);
  }

} // namespace ipc
