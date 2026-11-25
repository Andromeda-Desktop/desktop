#pragma once

#include <sys/socket.h>
#include <sys/types.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Local Unix sockets (compositor, shell, dmenu, askpass, indexer): the one owner of peer identity, capped reads,
// client caps and deadline writes. libc and the standard library only, so every binary can compile it.
namespace ipc {

  using Deadline = std::chrono::steady_clock::time_point;

  class Fd {
  public:
    Fd() = default;
    explicit Fd(int fd) : m_fd(fd) {}
    Fd(Fd&& other) noexcept : m_fd(other.release()) {}
    Fd& operator=(Fd&& other) noexcept;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd();

    [[nodiscard]] int get() const { return m_fd; }
    [[nodiscard]] int release();
    [[nodiscard]] explicit operator bool() const { return m_fd >= 0; }

  private:
    int m_fd = -1;
  };

  // A connection from a process of this user. pidfd pins that process (SO_PEERPIDFD, Linux >= 6.5): pid alone can
  // name a new process after the peer exits.
  struct Peer {
    Fd conn;
    pid_t pid = 0;
    gid_t gid = 0; // the primary group at connect(), as SO_PEERCRED
    Fd pidfd;
  };

  // True while the process of `pidfd` runs. Check it after a lookup by pid, so the answer still names that process.
  [[nodiscard]] bool running(int pidfd);

  // Checks a connected socket: another user or no pidfd closes it and returns nothing (errno EACCES).
  [[nodiscard]] std::optional<Peer> checkPeer(Fd conn);

  // Accepts one connection (flags as accept4) and checks it as checkPeer; errno tells EAGAIN (no connection
  // waits) apart. Close-on-exec always.
  [[nodiscard]] std::optional<Peer> acceptPeer(int listenFd, int flags);

  // Splits a byte stream into '\n' lines and holds at most cap bytes: a line that does not fit in cap bytes with
  // its '\n' ends the stream.
  class LineReader {
  public:
    enum class State { Reading, TooLong, Closed };

    explicit LineReader(std::size_t cap) : m_cap(cap) {}

    // One read() from fd into the free space; returns the bytes read, 0 when none came. EAGAIN keeps Reading; end
    // of file or an error gives Closed; a full buffer with no '\n' gives TooLong and drops the buffer. TooLong and
    // Closed are final. Call nextLine() until it is empty before the next read. In TooLong it reads and drops up to
    // cap more bytes: a socket closed with unread bytes resets, and the peer would lose the error reply.
    std::size_t readFrom(int fd);
    // The next complete line without its '\n'. Lines read before Closed stay available.
    [[nodiscard]] std::optional<std::string> nextLine();
    // After Closed and the last nextLine(): the bytes after the last '\n', for a peer that ends its last line with
    // end of file.
    [[nodiscard]] std::string takeRest();
    [[nodiscard]] bool hasLine() const { return m_buffer.find('\n', m_start) != std::string::npos; }
    [[nodiscard]] State state() const { return m_state; }
    [[nodiscard]] std::size_t buffered() const { return m_buffer.size() - m_start; }

  private:
    std::size_t m_cap;
    std::string m_buffer;
    std::size_t m_start = 0;
    std::size_t m_dropped = 0;
    State m_state = State::Reading;
  };

  // Bounds the open connections of one server. The slots object outlives every Slot it gives.
  class ClientSlots {
  public:
    class Slot {
    public:
      Slot(Slot&& other) noexcept : m_owner(other.m_owner) { other.m_owner = nullptr; }
      Slot& operator=(Slot&& other) noexcept;
      Slot(const Slot&) = delete;
      Slot& operator=(const Slot&) = delete;
      ~Slot();

    private:
      friend class ClientSlots;
      explicit Slot(ClientSlots* owner) : m_owner(owner) {}
      ClientSlots* m_owner;
    };

    explicit ClientSlots(std::size_t max) : m_max(max) {}
    [[nodiscard]] std::optional<Slot> acquire();
    [[nodiscard]] std::size_t used() const { return m_used; }

  private:
    std::size_t m_max;
    std::size_t m_used = 0;
  };

  enum class Status { Ok, NoServer, TimedOut, TooLong, Failed };

  struct Read {
    Status status = Status::Failed;
    std::string data;
  };

  // Writes all bytes, waiting for space until the deadline (then errno ETIMEDOUT). Works on blocking and non-blocking
  // fds; never SIGPIPE.
  [[nodiscard]] bool writeAll(int fd, std::string_view bytes, Deadline deadline);

  // Reads until end of file, at most cap bytes, until the deadline (Deadline::max(): none); complete (when set) ends the
  // read early once it accepts the data (a peer that keeps its end open). data holds what came, also on TooLong and
  // TimedOut.
  [[nodiscard]] Read readAll(int fd, std::size_t cap, Deadline deadline, bool (*complete)(std::string_view) = nullptr);

  // A connected socket (close-on-exec), or an empty Fd with errno: no socket at path, or the server's backlog stayed
  // full until the deadline. type: SOCK_STREAM or SOCK_SEQPACKET; | SOCK_NONBLOCK makes the connected fd non-blocking.
  // A deadline already past connects without waiting; else blocking sends keep the deadline's time left as SO_SNDTIMEO.
  [[nodiscard]] Fd connectTo(const std::string& path, Deadline deadline, int type = SOCK_STREAM);

  // Keep: an existing path fails (EADDRINUSE), so another process's socket is never replaced. Replace: unlinks it.
  enum class Stale { Keep, Replace };

  // A listening Unix socket at path. type: SOCK_STREAM or SOCK_SEQPACKET, optionally | SOCK_NONBLOCK; close-on-exec
  // always. mode 0 keeps the umask's. An empty Fd with errno on failure; a failure after bind removes the path again.
  [[nodiscard]] Fd listenUnix(const std::string& path, int type, int backlog, mode_t mode, Stale stale);

  // One sendmsg of bytes with fds attached (SCM_RIGHTS), never SIGPIPE; waits for room until the deadline (a past one:
  // no wait). The bytes sent, or -1 with errno (EAGAIN or ETIMEDOUT: no room). On a stream socket the fds go with the
  // first byte; a datagram socket sends all bytes or none.
  // At most kMaxFds fds per message (else -1, EINVAL).
  [[nodiscard]] ssize_t sendFds(int fd, std::string_view bytes, std::span<const int> fds, Deadline deadline);

  inline constexpr std::size_t kMaxFds = 253; // the kernel's SCM_MAX_FD

  struct Received {
    enum class Kind : std::uint8_t { Data, WouldBlock, Closed, Truncated, Failed };
    Kind kind = Kind::Failed;
    std::size_t size = 0;
    std::vector<Fd> fds;
  };

  // One recvmsg into buffer, never waiting. fds come close-on-exec, at most maxFds (<= kMaxFds). Closed: end of file
  // (or an empty datagram). Truncated: the bytes or the fds did not fit; fds that came still close. Other control
  // messages (SO_PASSCRED) are ignored. Failed keeps errno.
  [[nodiscard]] Received recvFds(int fd, std::span<std::byte> buffer, std::size_t maxFds);

  enum class EndOfRequest { KeepOpen, HalfClose };

  // One request: connect, write bytes, optionally shut down the write side, then read the reply as readAll does.
  [[nodiscard]] Read request(const std::string& path, std::string_view bytes, EndOfRequest end, std::size_t cap,
                             Deadline deadline, bool (*complete)(std::string_view) = nullptr);

} // namespace ipc
