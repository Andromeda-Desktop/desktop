#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

class SessionBus;

namespace sdbus {
  class IProxy;
}

namespace term {

  // Desktop notifications (org.freedesktop.Notifications) with a default action: clicking one runs its callback
  // with the xdg-activation token the server sent.
  class Notifier {
  public:
    explicit Notifier(SessionBus* bus);
    ~Notifier();
    Notifier(const Notifier&) = delete;
    Notifier& operator=(const Notifier&) = delete;

    void notify(const std::string& summary, const std::string& body, std::function<void(const std::string&)> onClick);

  private:
    std::unique_ptr<sdbus::IProxy> m_proxy;
    std::map<std::uint32_t, std::function<void(const std::string&)>> m_onClick; // by notification id
    std::map<std::uint32_t, std::string> m_tokens;                               // ActivationToken before ActionInvoked
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace term
