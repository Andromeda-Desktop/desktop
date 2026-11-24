#include "index_search.h"

#include "core/blocking_pool.h"
#include "core/deferred_call.h"
#include "core/files/file_info.h"
#include "core/log.h"

#include <algorithm>
#include <filesystem>
#include <thread>
#include <utility>

namespace {

  constexpr Logger kLog("files");
  constexpr int kLimit = 200;
  constexpr std::chrono::milliseconds kSearchDebounce{60}; // typing faster than this sends one query

} // namespace

IndexSearch::~IndexSearch() { disconnect(); }

bool IndexSearch::available() {
  const std::string path = IndexClient::socketPath();
  std::error_code ec;
  return !path.empty() && std::filesystem::exists(path, ec);
}

void IndexSearch::disconnect() {
  m_client.close();
  m_asked.clear();
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
  if (!m_client.query(kLimit, text, kind)) {
    kLog.debug("andromeda-index not reachable");
    for (Asked& asked : std::exchange(m_asked, {})) {
      answered(std::move(asked), {});
    }
    done(text, {});
    return;
  }
  m_asked.push_back({.text = text, .done = std::move(done), .search = search});
}

void IndexSearch::doAddPollFds(std::vector<pollfd>& fds) {
  if (m_client.fd() >= 0) {
    fds.push_back({.fd = m_client.fd(), .events = m_client.pollEvents(), .revents = 0});
  }
}

void IndexSearch::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (m_client.fd() < 0 || startIdx >= fds.size() || fds[startIdx].fd != m_client.fd()) {
    return;
  }
  const bool open = m_client.dispatch(fds[startIdx].revents, [this](std::vector<std::string> rows) {
    if (m_asked.empty()) {
      return;
    }
    Asked asked = std::move(m_asked.front());
    m_asked.pop_front();
    std::vector<std::string> paths;
    if (!superseded(asked)) {
      paths.reserve(rows.size());
      for (std::string& row : rows) {
        row.resize(std::min(row.find('\t'), row.size())); // "path\tmtime_ns\tsize\tkind\tmatch"
        paths.push_back(std::move(row));
      }
    }
    answered(std::move(asked), std::move(paths));
  });
  if (!open) {
    for (Asked& asked : std::exchange(m_asked, {})) {
      answered(std::move(asked), {});
    }
  }
}

void IndexSearch::answered(Asked asked, std::vector<std::string> paths) {
  const std::uint64_t answer = m_answers++;
  const bool stat = !superseded(asked) && !paths.empty();
  m_statting.emplace(answer, std::move(asked));
  if (!stat) {
    deliver(answer, {});
    return;
  }
  BlockingPool::instance().run([this, alive = std::weak_ptr<bool>(m_alive), answer, paths = std::move(paths)] {
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
  });
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
