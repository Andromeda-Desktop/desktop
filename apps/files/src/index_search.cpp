#include "index_search.h"

#include "core/deferred_call.h"
#include "core/files/file_info.h"
#include "core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {

  constexpr Logger kLog("files");
  constexpr int kLimit = 200;
  constexpr std::chrono::milliseconds kSearchDebounce{60}; // typing faster than this sends one query

  std::string socketPath() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    return runtime != nullptr && runtime[0] != '\0' ? std::string(runtime) + "/andromeda-index.sock" : std::string();
  }

} // namespace

IndexSearch::~IndexSearch() { disconnect(); }

bool IndexSearch::available() {
  const std::string path = socketPath();
  std::error_code ec;
  return !path.empty() && std::filesystem::exists(path, ec);
}

bool IndexSearch::connect() {
  if (m_fd >= 0) {
    return true;
  }
  const std::string path = socketPath();
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
    return false;
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return false;
  }
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    kLog.debug("andromeda-index not reachable: {}", std::strerror(errno));
    close(fd);
    return false;
  }
  m_fd = fd;
  return true;
}

void IndexSearch::disconnect() {
  if (m_fd >= 0) {
    close(m_fd);
  }
  m_fd = -1;
  m_asked.clear();
  m_out.clear();
  m_in.clear();
  m_paths.clear();
}

void IndexSearch::query(const std::string& text, std::string_view kind, Answer done) {
  send(text, kind, std::move(done), 0);
}

void IndexSearch::search(const std::string& text, std::string_view kind, Answer done) {
  m_debounce.start(kSearchDebounce, [this, text, kind = std::string(kind), done = std::move(done)]() mutable {
    send(text, kind, std::move(done), ++m_searches);
  });
}

void IndexSearch::send(const std::string& text, std::string_view kind, Answer done, std::uint64_t search) {
  if (!connect()) {
    done(text, {});
    return;
  }
  // The protocol is one line per request: no newlines in the text.
  std::string words = text;
  std::ranges::replace(words, '\n', ' ');
  m_out += std::format("query {} {}{}{}\n", kLimit, words, kind.empty() ? "" : " kind:", kind);
  m_asked.push_back({.text = text, .done = std::move(done), .search = search});
  flush();
}

void IndexSearch::flush() {
  while (!m_out.empty()) {
    const ssize_t n = ::send(m_fd, m_out.data(), m_out.size(), MSG_NOSIGNAL);
    if (n < 0) {
      if (errno != EAGAIN && errno != EINTR) {
        disconnect();
      }
      return;
    }
    m_out.erase(0, static_cast<std::size_t>(n));
  }
}

void IndexSearch::doAddPollFds(std::vector<pollfd>& fds) {
  if (m_fd >= 0) {
    fds.push_back({.fd = m_fd, .events = static_cast<short>(POLLIN | (m_out.empty() ? 0 : POLLOUT)), .revents = 0});
  }
}

void IndexSearch::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (m_fd < 0 || startIdx >= fds.size() || fds[startIdx].fd != m_fd || fds[startIdx].revents == 0) {
    return;
  }
  if ((fds[startIdx].revents & POLLOUT) != 0) {
    flush();
  }
  char buf[16384];
  for (;;) {
    const ssize_t n = ::read(m_fd, buf, sizeof(buf));
    if (n > 0) {
      m_in.append(buf, static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
      auto asked = std::move(m_asked);
      disconnect();
      for (Asked& a : asked) {
        answered(std::move(a), {});
      }
      return;
    }
    break;
  }
  std::size_t start = 0;
  for (std::size_t nl = m_in.find('\n'); nl != std::string::npos; nl = m_in.find('\n', start)) {
    const std::string_view line(m_in.data() + start, nl - start);
    start = nl + 1;
    if (line.empty()) { // the end of one answer
      if (!m_asked.empty()) {
        Asked asked = std::move(m_asked.front());
        m_asked.pop_front();
        answered(std::move(asked), std::move(m_paths));
      }
      m_paths.clear();
      continue;
    }
    // "path\tmtime_ns\tsize\tkind\tmatch"
    if (!m_asked.empty() && !superseded(m_asked.front())) {
      m_paths.emplace_back(line.substr(0, line.find('\t')));
    }
  }
  m_in.erase(0, start);
}

void IndexSearch::answered(Asked asked, std::vector<std::string> paths) {
  const std::uint64_t answer = m_answers++;
  const bool stat = !superseded(asked) && !paths.empty();
  m_statting.emplace(answer, std::move(asked));
  if (!stat) {
    deliver(answer, {});
    return;
  }
  std::thread([this, alive = std::weak_ptr<bool>(m_alive), answer, paths = std::move(paths)] {
    Rows rows;
    for (const std::string& path : paths) {
      if (auto info = statFile(std::filesystem::path(path))) {
        info->setNameKeepLabel(path);
        rows.push_back(std::make_shared<const FileInfo>(std::move(*info)));
      }
    }
    DeferredCall::callLater([this, alive, answer, rows = std::move(rows)]() mutable {
      if (!alive.expired()) {
        deliver(answer, std::move(rows));
      }
    });
  }).detach();
}

void IndexSearch::deliver(std::uint64_t answer, Rows rows) {
  m_statted.emplace(answer, std::move(rows));
  for (auto it = m_statted.find(m_delivered); it != m_statted.end(); it = m_statted.find(m_delivered)) {
    Rows ready = std::move(it->second);
    m_statted.erase(it);
    auto node = m_statting.extract(m_delivered);
    ++m_delivered;
    if (!superseded(node.mapped())) {
      node.mapped().done(node.mapped().text, std::move(ready));
    }
  }
}
