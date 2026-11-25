#include "query_server.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>

namespace andromeda::index {

  namespace {
    constexpr std::size_t kMaxLine = 4096;
    constexpr std::size_t kContentRows = 3;
    constexpr long kMinTextQuery = 4;
    constexpr std::string_view kKindPrefix = "kind:";
    constexpr int kFirstPassedFd = 3; // SD_LISTEN_FDS_START

    int passedSocket() {
      const char* pid = std::getenv("LISTEN_PID");
      const char* count = std::getenv("LISTEN_FDS");
      if (pid == nullptr || count == nullptr || std::atoi(pid) != getpid() || std::atoi(count) < 1) {
        return -1;
      }
      // systemd passes a blocking socket; accept() must not block the loop.
      fcntl(kFirstPassedFd, F_SETFL, fcntl(kFirstPassedFd, F_GETFL) | O_NONBLOCK);
      fcntl(kFirstPassedFd, F_SETFD, FD_CLOEXEC);
      return kFirstPassedFd;
    }
  } // namespace

  QueryServer::QueryServer(const IndexDb& db, int listenFd) : m_db(db), m_listen(listenFd) {}

  QueryServer::~QueryServer() {
    m_clients.clear();
    if (m_listen >= 0) {
      close(m_listen);
    }
  }

  int QueryServer::listenSocket(const std::filesystem::path& path) {
    if (const int fd = passedSocket(); fd >= 0) {
      return fd;
    }
    return ipc::listenUnix(path.string(), SOCK_STREAM | SOCK_NONBLOCK, 16, 0, ipc::Stale::Replace).release();
  }

  void QueryServer::addPollFds(std::vector<pollfd>& fds) const {
    fds.push_back({.fd = m_listen, .events = POLLIN, .revents = 0});
    for (const auto& [fd, client] : m_clients) {
      fds.push_back({.fd = fd, .events = POLLIN, .revents = 0});
    }
  }

  void QueryServer::handle(std::span<const pollfd> fds) {
    for (const pollfd& p : fds) {
      if (p.revents == 0) {
        continue;
      }
      if (p.fd == m_listen) {
        accept();
      } else if (!serve(p.fd)) {
        m_clients.erase(p.fd);
      }
    }
  }

  void QueryServer::accept() {
    while (true) {
      auto peer = ipc::acceptPeer(m_listen, SOCK_NONBLOCK);
      if (!peer) {
        if (errno == EACCES) {
          continue; // another user, or a peer without a pidfd
        }
        return;
      }
      auto slot = m_slots.acquire();
      if (!slot) {
        continue; // the client sees end of file
      }
      const int fd = peer->conn.get();
      m_clients.emplace(fd, Client{.conn = std::move(peer->conn), .reader = ipc::LineReader(kMaxLine), .slot = std::move(*slot)});
    }
  }

  bool QueryServer::serve(int fd) {
    ipc::LineReader& reader = m_clients.at(fd).reader;
    // A client that half-closes after its request still gets the answer.
    std::string out;
    do {
      while (auto line = reader.nextLine()) {
        out += answer(*line);
      }
    } while (reader.readFrom(fd) > 0);
    if (reader.state() == ipc::LineReader::State::TooLong) {
      return false;
    }
    // Answers are at most kMaxLimit rows and fit the socket buffer; a client that does not read is dropped
    // rather than allowed to block the daemon.
    const bool sent = out.empty() || send(fd, out.data(), out.size(), MSG_NOSIGNAL | MSG_DONTWAIT) == static_cast<ssize_t>(out.size());
    return sent && reader.state() == ipc::LineReader::State::Reading;
  }

  std::string QueryServer::answer(std::string_view line) const {
    constexpr std::string_view kQuery = "query ";
    if (!line.starts_with(kQuery)) {
      return "\n";
    }
    line.remove_prefix(kQuery.size());
    int limit = 0;
    const auto [rest, error] = std::from_chars(line.data(), line.data() + line.size(), limit);
    if (error != std::errc{} || rest == line.data() + line.size() || *rest != ' ') {
      return "\n";
    }
    // A kind:<word> token filters by kind; the other words are the text.
    std::string text;
    FileKind kind = FileKind::Any;
    for (std::string_view words = line.substr(static_cast<std::size_t>(rest - line.data()) + 1); !words.empty();) {
      const std::string_view word = words.substr(0, words.find(' '));
      words.remove_prefix(std::min(words.size(), word.size() + 1));
      if (word.empty()) {
        continue;
      }
      if (!word.starts_with(kKindPrefix)) {
        text += text.empty() ? "" : " ";
        text += word;
        continue;
      }
      const auto parsed = parseKind(word.substr(kKindPrefix.size()));
      if (!parsed) {
        return "\n";
      }
      kind = *parsed;
    }
    limit = std::clamp(limit, 1, kMaxLimit);
    std::vector<FileRow> names = m_db.queryNames(text, limit, kind);
    std::vector<std::string> named;
    named.reserve(names.size());
    for (const FileRow& row : names) {
      named.push_back(row.path);
    }
    // Three letters match words in nearly every document: noise that costs ~8 ms per query.
    const bool searchText = std::ranges::count_if(text, [](char c) { return std::isspace(static_cast<unsigned char>(c)) == 0; })
        >= kMinTextQuery;
    const bool kindHasText = kind != FileKind::Folder && kind != FileKind::Video;
    std::vector<FileRow> content =
        searchText && kindHasText ? m_db.queryContent(text, kind == FileKind::Any ? limit : kMaxLimit, named) : std::vector<FileRow>{};
    if (kind != FileKind::Any) {
      std::erase_if(content, [kind](const FileRow& row) { return fileKind(row.name, row.isDir) != kind; });
    }
    // Names first; up to kContentRows rows stay free for files that match by their text.
    names.resize(std::min(names.size(), static_cast<std::size_t>(limit) - std::min(content.size(), kContentRows)));
    std::string out;
    const auto add = [&out](const FileRow& row, char match) {
      // A path with a tab or a newline cannot be framed in this protocol.
      if (row.path.find_first_of("\t\n") != std::string::npos) {
        return;
      }
      out += row.path;
      out += '\t';
      out += std::to_string(row.mtime);
      out += '\t';
      out += std::to_string(row.size);
      out += row.isDir ? "\td\t" : "\tf\t";
      out += match;
      out += '\n';
    };
    for (const FileRow& row : names) {
      add(row, 'n');
    }
    for (std::size_t i = 0; names.size() + i < static_cast<std::size_t>(limit) && i < content.size(); ++i) {
      add(content[i], 'c');
    }
    out += '\n';
    return out;
  }

} // namespace andromeda::index
