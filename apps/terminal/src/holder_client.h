#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

// Client side of the andromeda-ptyd wire format (apps/terminal/ptyd/src/proto.rs).
namespace term::holder {

struct WinSize {
  std::uint16_t cols = 0;
  std::uint16_t rows = 0;
  std::uint16_t xpixel = 0;
  std::uint16_t ypixel = 0;
};

struct Spawned {
  std::uint32_t req;
  std::uint64_t id;
};
struct SpawnFailed {
  std::uint32_t req;
  std::string message;
};
struct Attached {
  std::uint64_t id;
  std::uint64_t offset;
  std::vector<std::uint8_t> snapshot;
  std::vector<std::uint8_t> tail;
};
struct AttachFailed {
  std::uint64_t id;
};
struct Output {
  std::uint64_t id;
  std::uint64_t offset;
  std::vector<std::uint8_t> bytes;
};
struct WantSnapshot {
  std::uint64_t id;
};
struct Exited {
  std::uint64_t id;
  int status; // raw wait status
};
struct Orphans {
  std::vector<std::uint64_t> ids;
};

using Message = std::variant<Spawned, SpawnFailed, Attached, AttachFailed, Output, WantSnapshot, Exited, Orphans>;

std::vector<std::uint8_t> encodeSpawn(std::uint32_t req, WinSize size, const std::string& cwd,
                                      const std::vector<std::string>& argv, const std::vector<std::string>& env);
std::vector<std::uint8_t> encodeAttach(std::uint64_t id);
std::vector<std::uint8_t> encodeSnapshot(std::uint64_t id, std::uint64_t offset, std::span<const std::uint8_t> blob);
std::vector<std::uint8_t> encodeClose(std::uint64_t id);
std::vector<std::uint8_t> encodeList();

// Splits one frame off the front of `buf`. Returns the bytes used, 0 when the frame is incomplete,
// or nullopt when the frame is malformed.
std::optional<std::size_t> decode(std::span<const std::uint8_t> buf, std::optional<Message>& out);

// $ANDROMEDA_PTYD_SOCKET, else $XDG_RUNTIME_DIR/andromeda/ptyd-1.sock.
std::string socketPath();

// Blocking connection with a receive timeout for the handshake; -1 when the holder is unreachable.
int connectSocket();

bool sendAll(int fd, std::span<const std::uint8_t> bytes);

// Reads into `buf` and appends a received SCM_RIGHTS fd to `fds`. Returns bytes read, 0 at EOF, -1 on error
// (errno set; EAGAIN when `nonblocking` and nothing is ready).
long receive(int fd, std::vector<std::uint8_t>& buf, std::vector<int>& fds, bool nonblocking);

// Blocks until one message arrives (up to the socket's receive timeout). Takes the pty fd of Spawned and
// Attached into `fd`.
std::optional<Message> receiveOne(int sock, std::vector<std::uint8_t>& buf, int& fd);

// Orphaned session ids, or an empty list when the holder is unreachable.
std::vector<std::uint64_t> listOrphans();

} // namespace term::holder
