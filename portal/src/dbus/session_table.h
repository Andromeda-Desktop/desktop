#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sdbus {
  class IConnection;
} // namespace sdbus

namespace xdpu {

  class Loop;
  class Session;

  // Every portal session by handle. ScreenCast and RemoteDesktop share it: a RemoteDesktop session may also select
  // ScreenCast sources.
  class SessionTable {
  public:
    using ClosedListener = std::function<void(Session&)>;

    SessionTable(Loop& loop, sdbus::IConnection& connection);
    ~SessionTable();

    SessionTable(const SessionTable&) = delete;
    SessionTable& operator=(const SessionTable&) = delete;

    // Closes a session already at `path` from the same owner; nullptr when another owner holds `path` open.
    std::shared_ptr<Session> create(const std::string& path, const std::string& appId, const std::string& owner);
    // nullptr when no open session has this handle.
    [[nodiscard]] std::shared_ptr<Session> find(const std::string& path) const;
    // nullptr unless `owner` created the open session at `path`.
    [[nodiscard]] std::shared_ptr<Session> find(const std::string& path, const std::string& owner) const;
    [[nodiscard]] const std::map<std::string, std::shared_ptr<Session>>& sessions() const;
    // Runs right after a session closes, before the table drops it.
    void addClosedListener(ClosedListener listener);

  private:
    Loop& m_loop;
    sdbus::IConnection& m_connection;
    std::map<std::string, std::shared_ptr<Session>> m_sessions;
    std::vector<ClosedListener> m_closedListeners;
  };

} // namespace xdpu
