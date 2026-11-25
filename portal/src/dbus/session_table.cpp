#include "dbus/session_table.h"

#include "dbus/session.h"
#include "loop/loop.h"

namespace xdpu {

  SessionTable::SessionTable(Loop& loop, sdbus::IConnection& connection) : m_loop(loop), m_connection(connection) {}

  SessionTable::~SessionTable() {
    m_closedListeners.clear();
    m_sessions.clear();
  }

  std::shared_ptr<Session>
  SessionTable::create(const std::string& path, const std::string& appId, const std::string& owner) {
    if (const auto existing = m_sessions.find(path); existing != m_sessions.end()) {
      const auto session = existing->second;
      if (!session->closed() && session->owner() != owner) {
        return nullptr;
      }
      session->close();
      m_sessions.erase(path);
    }
    auto session = std::make_shared<Session>(m_connection, path, appId, owner, [this, path]() {
      if (const auto it = m_sessions.find(path); it != m_sessions.end() && it->second->closed()) {
        const auto closed = it->second;
        for (const auto& listener : std::vector(m_closedListeners)) {
          listener(*closed);
        }
        // Erase later: the session is still on the stack of its own Close handler.
        m_loop.addTimer(0, [this, path]() {
          if (const auto it = m_sessions.find(path); it != m_sessions.end() && it->second->closed()) {
            m_sessions.erase(it);
          }
        });
      }
    });
    m_sessions.emplace(path, session);
    return session;
  }

  std::shared_ptr<Session> SessionTable::find(const std::string& path) const {
    const auto it = m_sessions.find(path);
    return it == m_sessions.end() || it->second->closed() ? nullptr : it->second;
  }

  std::shared_ptr<Session> SessionTable::find(const std::string& path, const std::string& owner) const {
    auto session = find(path);
    return session && session->owner() == owner ? session : nullptr;
  }

  const std::map<std::string, std::shared_ptr<Session>>& SessionTable::sessions() const { return m_sessions; }

  void SessionTable::addClosedListener(ClosedListener listener) { m_closedListeners.push_back(std::move(listener)); }

} // namespace xdpu
