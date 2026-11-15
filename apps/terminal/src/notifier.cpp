#include "notifier.h"

#include "core/log.h"
#include "dbus/session_bus.h"

#include <sdbus-c++/sdbus-c++.h>

#include <vector>

namespace term {

  namespace {
    constexpr Logger kLog("terminal-notify");
    constexpr auto kInterface = "org.freedesktop.Notifications";
  } // namespace

  Notifier::Notifier(SessionBus* bus) {
    if (bus == nullptr || !bus->connected()) {
      return;
    }
    try {
      m_proxy = sdbus::createProxy(bus->connection(), sdbus::ServiceName{kInterface},
                                   sdbus::ObjectPath{"/org/freedesktop/Notifications"});
      m_proxy->uponSignal("ActivationToken").onInterface(kInterface).call([this](std::uint32_t id, const std::string& token) {
        if (m_onClick.contains(id)) {
          m_tokens[id] = token;
        }
      });
      m_proxy->uponSignal("ActionInvoked").onInterface(kInterface).call([this](std::uint32_t id, const std::string& key) {
        auto it = m_onClick.find(id);
        if (it == m_onClick.end() || key != "default") {
          return;
        }
        const auto token = m_tokens.extract(id);
        auto onClick = std::move(it->second);
        m_onClick.erase(it);
        onClick(token.empty() ? std::string{} : token.mapped());
      });
      m_proxy->uponSignal("NotificationClosed").onInterface(kInterface).call([this](std::uint32_t id, std::uint32_t) {
        m_onClick.erase(id);
        m_tokens.erase(id);
      });
    } catch (const sdbus::Error& e) {
      kLog.warn("notifications unavailable: {}", e.what());
      m_proxy.reset();
    }
  }

  Notifier::~Notifier() { m_alive.reset(); }

  void Notifier::notify(const std::string& summary, const std::string& body,
                        std::function<void(const std::string&)> onClick) {
    if (m_proxy == nullptr) {
      return;
    }
    const std::vector<std::string> actions{"default", "Show"};
    const std::map<std::string, sdbus::Variant> hints{{"desktop-entry", sdbus::Variant{std::string{"dev.andromeda.Terminal"}}}};
    try {
      m_proxy->callMethodAsync("Notify")
          .onInterface(kInterface)
          .withArguments(std::string{"Terminal"}, std::uint32_t{0}, std::string{"dev.andromeda.Terminal"}, summary, body,
                         actions, hints, std::int32_t{-1})
          .uponReplyInvoke([this, alive = std::weak_ptr<bool>(m_alive), onClick = std::move(onClick)](
                               std::optional<sdbus::Error> error, std::uint32_t id) {
            if (alive.expired()) {
              return;
            }
            if (error) {
              kLog.warn("notify: {}", error->what());
              return;
            }
            m_onClick[id] = onClick;
          });
    } catch (const sdbus::Error& e) {
      kLog.warn("notify: {}", e.what());
    }
  }

} // namespace term
