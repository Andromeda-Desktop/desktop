#pragma once

#include <cstdint>
#include <map>
#include <memory>

class SessionBus;

namespace sdbus {
  class IObject;
}

// org.freedesktop.impl.portal.Email, served by `andromeda-files --picker`. ComposeEmail hands the message to
// xdg-email, which opens the user's mail app (with attachments for the apps it knows) and answers when it exits.
class EmailPortal {
public:
  explicit EmailPortal(SessionBus& bus);
  ~EmailPortal();
  EmailPortal(const EmailPortal&) = delete;
  EmailPortal& operator=(const EmailPortal&) = delete;

  [[nodiscard]] bool idle() const noexcept { return m_pending.empty(); }

private:
  struct Pending;

  SessionBus& m_bus;
  std::unique_ptr<sdbus::IObject> m_object;
  std::map<std::uint64_t, std::unique_ptr<Pending>> m_pending;
  std::uint64_t m_nextId = 1;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
