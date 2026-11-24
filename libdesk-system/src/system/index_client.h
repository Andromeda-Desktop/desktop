#pragma once

#include "protocols/ipc_channel.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// The one client of the andromeda-index query socket ($XDG_RUNTIME_DIR/andromeda-index.sock, protocol in
// indexer/src/query_server.h), for an event loop: one non-blocking connection, answers in request order.
class IndexClient {
public:
  // One answer: its rows, each "path\tmtime_ns\tsize\tkind\tmatch".
  using Answer = std::function<void(std::vector<std::string> rows)>;

  // Empty without XDG_RUNTIME_DIR.
  [[nodiscard]] static std::string socketPath();
  // The daemon accepts a connection (the setup wizard's probe).
  [[nodiscard]] static bool reachable();

  // Queues one query for at most limit rows of words (newlines become spaces), of that kind when kind is set.
  // False when the daemon cannot be reached.
  bool query(int limit, std::string_view words, std::string_view kind);
  [[nodiscard]] int fd() const { return m_fd.get(); }
  // POLLIN, and POLLOUT while queued bytes wait for space.
  [[nodiscard]] short pollEvents() const;
  // Writes queued bytes, reads, and gives each complete answer to done, oldest first. False when the daemon went
  // away: the client is then closed and the queries without an answer get none.
  bool dispatch(short revents, const Answer& done);
  void close();

private:
  bool flush();

  ipc::Fd m_fd;
  std::string m_out;
  ipc::LineReader m_in{kMaxBuffered};
  std::vector<std::string> m_rows; // the answer being read

  static constexpr std::size_t kMaxBuffered = 1024 * 1024;
};
