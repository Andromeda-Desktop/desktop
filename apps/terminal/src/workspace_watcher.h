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
  // edits. Runs on the app loop and never blocks it. Workspaces live only in the holder: a workspace exists while a
  // window shows it or a shell in it runs, so the first table of each connection closes the ones with no shell.
  class WorkspaceWatcher final : public PollSource {
  public:
    explicit WorkspaceWatcher(std::function<void()> changed);
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
    // Ends the workspace's shells and removes it from the holder.
    void close(std::uint64_t workspace);

    void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

  protected:
    void doAddPollFds(std::vector<pollfd>& fds) override;

  private:
    void send(std::vector<std::uint8_t> frame);
    void flush();
    void read();
    void disconnect();
    void closeEmpty();

    std::function<void()> m_changed;
    int m_fd = -1; // -1: not connected
    std::vector<std::uint8_t> m_in;
    std::vector<std::uint8_t> m_out;
    std::optional<std::uint64_t> m_seq; // last applied on this connection
    holder::Workspaces m_table;
    std::uint32_t m_nextReq = 1;
    std::map<std::uint32_t, std::function<void(std::uint32_t, std::uint64_t)>> m_puts; // by request id
  };

} // namespace term
