#pragma once

#include "core/poll_source.h"
#include "holder_client.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace term {

  // The app's one Watch connection to andromeda-ptyd: keeps the holder's latest workspace table and sends workspace
  // edits. Runs on the app loop and never blocks it.
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

    void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

  protected:
    void doAddPollFds(std::vector<pollfd>& fds) override;

  private:
    void send(std::vector<std::uint8_t> frame);
    void flush();
    void read();
    void disconnect();

    std::function<void()> m_changed;
    int m_fd = -1; // -1: not connected
    std::vector<std::uint8_t> m_in;
    std::vector<std::uint8_t> m_out;
    std::optional<std::uint64_t> m_seq; // last applied on this connection
    holder::Workspaces m_table;
  };

} // namespace term
