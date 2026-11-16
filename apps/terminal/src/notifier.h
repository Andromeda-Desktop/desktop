#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "osc99.h"

class SessionBus;

namespace sdbus {
  class IProxy;
}

namespace term {

  // Desktop notifications (org.freedesktop.Notifications). A click runs onAction with the xdg-activation token the
  // server sent and the button (0: the body, n: Notice::buttons[n - 1]). A non-empty key replaces or closes an
  // earlier notification with the same key.
  class Notifier {
  public:
    explicit Notifier(SessionBus* bus);
    ~Notifier();
    Notifier(const Notifier&) = delete;
    Notifier& operator=(const Notifier&) = delete;

    using ActionFn = std::function<void(const std::string& token, int button)>;
    void notify(const Notice& notice, const std::string& key, ActionFn onAction, std::function<void()> onClosed = {});
    void close(const std::string& key);

  private:
    struct Entry {
      std::string key;
      ActionFn onAction;
      std::function<void()> onClosed;
    };
    void sendClose(std::uint32_t id);

    std::unique_ptr<sdbus::IProxy> m_proxy;
    std::map<std::uint32_t, Entry> m_entries;      // by notification id
    std::map<std::string, std::uint32_t> m_byKey;  // shown notifications with a key
    std::map<std::string, bool> m_inFlight;        // keys whose Notify has no reply yet: true when closed meanwhile
    std::map<std::uint32_t, std::string> m_tokens; // ActivationToken before ActionInvoked
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace term
