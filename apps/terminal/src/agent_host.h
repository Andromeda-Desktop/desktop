#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <sys/types.h>

#include "agent_kind.h"
#include "core/timer_manager.h"
#include "agent_session.h"

namespace term::agent {

  // One ACP server process per agent kind and app instance, shared by the sessions of that kind.
  // Reads and writes run on their own threads; every callback and model change runs on the UI thread
  // (DeferredCall). The host is the ACP client: it routes updates, permission requests and prompt replies
  // to the session models and sends what their outboxes hold.
  class Host {
  public:
    using Changed = std::function<void(std::uint64_t key)>;

    Host(Kind kind, Changed changed);
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // Opens a session in cwd; key identifies it to the caller. The model starts in Starting.
    Session& open(std::uint64_t key, const std::string& cwd);
    void close(std::uint64_t key);
    [[nodiscard]] Session* session(std::uint64_t key);

    // Call after a user action on a session model: sends its outbox.
    void flush(std::uint64_t key);
    void armHangCheck();

    [[nodiscard]] Kind kind() const { return m_kind; }

    // The agentd path: $ANDROMEDA_AGENTD, else the installed helper.
    [[nodiscard]] static std::string program();

  private:
    struct Slot {
      std::unique_ptr<Session> model;
      std::string sessionId;
      std::int64_t newId = 0;
    };
    struct Shared; // the write queue the writer thread shares with the UI thread

    bool ensureStarted();
    void request(const std::string& method, Json params, std::int64_t id);
    void write(const Json& message);
    void onMessage(const Json& message);
    void onExit(int status, std::string stderrTail);
    Slot* slotBySessionId(const std::string& id, std::uint64_t* key);

    Kind m_kind;
    Changed m_changed;
    std::map<std::uint64_t, Slot> m_slots;
    std::shared_ptr<Shared> m_shared;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0); // IO callbacks run only while this lives
    std::thread m_reader;
    std::thread m_writer;
    std::thread m_errors;
    pid_t m_pid = -1;
    bool m_initialized = false;
    std::int64_t m_initializeId = 0;
    std::int64_t m_nextId = 1'000'000; // above the ids the session models use
    // Hang check: a session waiting on the agent (Starting, Cancelling) with no message from it for kHangTimeout
    // ends the agent, so the exit path fails the session instead of leaving it waiting forever.
    Timer m_hangCheck;
  };

} // namespace term::agent
