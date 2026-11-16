#pragma once

#include "core/poll_source.h"
#include "core/timer_manager.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct FileInfo;

// Search This PC (FILES-UI-SPEC §15): name and content queries to the andromeda-index daemon over its socket
// ($XDG_RUNTIME_DIR/andromeda-index.sock, protocol in indexer/src/query_server.h). Answers come in request order;
// each reaches the callback of its request (an empty answer when the daemon is gone).
class IndexSearch final : public PollSource {
public:
  using Answer = std::function<void(const std::string& text, std::vector<std::shared_ptr<const FileInfo>> rows)>;

  IndexSearch() = default;
  ~IndexSearch() override;
  IndexSearch(const IndexSearch&) = delete;
  IndexSearch& operator=(const IndexSearch&) = delete;

  [[nodiscard]] static bool available(); // the daemon's socket exists
  // Empty text with a kind = the newest entries of that kind.
  void query(const std::string& text, std::string_view kind, Answer done);
  // A typed search: sent kSearchDebounce after the last call, and it supersedes every earlier search, whose
  // answers are then read without a stat and reach no callback.
  void search(const std::string& text, std::string_view kind, Answer done);

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  bool connect();
  void disconnect();
  void flush();

  struct Asked {
    std::string text;
    Answer done;
    std::uint64_t search; // 0 for query(); else this search's number, superseded once a later search is sent
  };
  using Rows = std::vector<std::shared_ptr<const FileInfo>>;
  void send(const std::string& text, std::string_view kind, Answer done, std::uint64_t search);
  [[nodiscard]] bool superseded(const Asked& asked) const { return asked.search != 0 && asked.search != m_searches; }
  // Answers are statted on a thread (a cold answer took 15-84 ms) and reach their callbacks in request order.
  void answered(Asked asked, std::vector<std::string> paths);
  void deliver(std::uint64_t answer, Rows rows);

  int m_fd = -1;
  std::deque<Asked> m_asked; // the unanswered requests, oldest first
  Timer m_debounce;
  std::uint64_t m_searches = 0; // searches sent
  std::string m_out;
  std::string m_in;
  std::vector<std::string> m_paths; // the answer being read
  std::uint64_t m_answers = 0;      // answers read
  std::uint64_t m_delivered = 0;    // answers delivered; the next one to deliver has this number
  std::map<std::uint64_t, Asked> m_statting;
  std::map<std::uint64_t, Rows> m_statted; // statted, waiting for an earlier answer
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
