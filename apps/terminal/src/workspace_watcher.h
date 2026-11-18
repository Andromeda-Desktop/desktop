#pragma once

#include "core/poll_source.h"
#include "holder_client.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <variant>
#include <vector>

namespace term {

  // The app's one Watch connection to andromeda-ptyd: keeps the holder's latest workspace table and sends workspace
  // edits. Runs on the app loop and never blocks it.
  // The holder forgets its workspaces when it ends, so the definitions (id 0) persist through load/save: the first
  // table of each connection gets the saved ones it lacks, and the table is saved only after those answers arrive.
  class WorkspaceWatcher final : public PollSource {
  public:
    using Defs = std::vector<holder::WorkspaceInfo>;
    WorkspaceWatcher(std::function<void()> changed, std::function<Defs()> load, std::function<void(const Defs&)> save);
    ~WorkspaceWatcher() override;
    WorkspaceWatcher(const WorkspaceWatcher&) = delete;
    WorkspaceWatcher& operator=(const WorkspaceWatcher&) = delete;

    // Connects and sends Watch when not connected; false when the holder is unreachable.
    bool connect();
    // The last table received; kept while the holder is unreachable.
    [[nodiscard]] const holder::Workspaces& workspaces() const { return m_table; }
    // Sends PutWorkspace (def.id 0 makes a new one); `done` gets the request id and the workspace id. Returns the
    // request id, or nullopt when the holder is unreachable. A lost connection drops the answer.
    std::optional<std::uint32_t> put(const holder::WorkspaceInfo& def,
                                     std::function<void(std::uint32_t req, std::uint64_t id)> done);
    void setSession(std::uint64_t heldId, std::uint64_t workspace);

    void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

  protected:
    void doAddPollFds(std::vector<pollfd>& fds) override;

  private:
    void send(std::vector<std::uint8_t> frame);
    void flush();
    void read();
    void disconnect();
    void syncDefs();

    struct AwaitTable {};
    struct Restoring {
      std::set<std::uint32_t> reqs; // PutWorkspace requests not answered yet
    };
    struct Live {};

    std::function<void()> m_changed;
    std::function<Defs()> m_load;
    std::function<void(const Defs&)> m_save;
    std::variant<AwaitTable, Restoring, Live> m_sync; // of this connection
    Defs m_saved; // what load returned or save last got
    int m_fd = -1; // -1: not connected
    std::vector<std::uint8_t> m_in;
    std::vector<std::uint8_t> m_out;
    std::optional<std::uint64_t> m_seq; // last applied on this connection
    holder::Workspaces m_table;
    std::uint32_t m_nextReq = 1;
    std::map<std::uint32_t, std::function<void(std::uint32_t, std::uint64_t)>> m_puts; // by request id
  };

} // namespace term
