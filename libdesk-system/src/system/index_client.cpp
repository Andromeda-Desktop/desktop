#include "protocols/xdg_dirs.h"
#include "system/index_client.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <format>
#include <poll.h>
#include <sys/socket.h>
#include <utility>

namespace {

  // Long enough for a busy daemon's backlog, short enough for the UI thread.
  constexpr auto kConnectWait = std::chrono::milliseconds(50);

  ipc::Fd connectToDaemon() {
    return ipc::connectTo(IndexClient::socketPath(), std::chrono::steady_clock::now() + kConnectWait);
  }

} // namespace

std::string IndexClient::socketPath() {
  const std::string runtime = xdg::runtimeDir();
  return runtime.empty() ? runtime : runtime + "/andromeda-index.sock";
}

bool IndexClient::reachable() { return static_cast<bool>(connectToDaemon()); }

bool IndexClient::query(int limit, std::string_view words, std::string_view kind) {
  if (!m_fd) {
    m_fd = connectToDaemon();
    if (!m_fd || fcntl(m_fd.get(), F_SETFL, fcntl(m_fd.get(), F_GETFL) | O_NONBLOCK) != 0) {
      close();
      return false;
    }
  }
  std::string text(words);
  std::ranges::replace(text, '\n', ' ');
  m_out += std::format("query {} {}{}{}\n", limit, kind.empty() ? "" : "kind:", kind, kind.empty() ? text : " " + text);
  if (!flush()) {
    close();
    return false;
  }
  return true;
}

short IndexClient::pollEvents() const { return static_cast<short>(POLLIN | (m_out.empty() ? 0 : POLLOUT)); }

bool IndexClient::flush() {
  while (!m_out.empty()) {
    const ssize_t n = ::send(m_fd.get(), m_out.data(), m_out.size(), MSG_NOSIGNAL);
    if (n < 0) {
      return errno == EAGAIN || errno == EINTR;
    }
    m_out.erase(0, static_cast<std::size_t>(n));
  }
  return true;
}

bool IndexClient::dispatch(short revents, const Answer& done) {
  if (!m_fd || revents == 0) {
    return static_cast<bool>(m_fd);
  }
  if ((revents & POLLOUT) != 0 && !flush()) {
    close();
    return false;
  }
  for (;;) {
    while (auto line = m_in.nextLine()) {
      if (line->empty()) { // the end of one answer
        done(std::exchange(m_rows, {}));
      } else {
        m_rows.push_back(std::move(*line));
      }
    }
    if (m_in.state() != ipc::LineReader::State::Reading || m_in.readFrom(m_fd.get()) == 0) {
      break;
    }
  }
  if (m_in.state() != ipc::LineReader::State::Reading) {
    close();
    return false;
  }
  return true;
}

void IndexClient::close() {
  m_fd = {};
  m_out.clear();
  m_in = ipc::LineReader(kMaxBuffered);
  m_rows.clear();
}
