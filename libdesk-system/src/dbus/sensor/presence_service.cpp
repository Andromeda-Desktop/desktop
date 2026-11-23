#include "dbus/sensor/presence_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"

#include <map>
#include <optional>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string>
#include <vector>

namespace {
  constexpr Logger kLog("presence");
  const sdbus::ServiceName kBusName{"org.andromeda.System1"};
  const sdbus::ObjectPath kPath{"/org/andromeda/System1/Sensors"};
  constexpr auto kInterface = "org.andromeda.System1.Sensors";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
} // namespace

PresenceService::PresenceService(SystemBus& bus, ChangeCallback onChange) : m_onChange(std::move(onChange)) {
  m_proxy = sdbus::createProxy(bus.connection(), kBusName, kPath);
  m_proxy->uponSignal("PropertiesChanged")
      .onInterface(kPropertiesInterface)
      .call([this](const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changed,
                   const std::vector<std::string>&) {
        if (interfaceName != kInterface) {
          return;
        }
        if (const auto it = changed.find("Presence"); it != changed.end() && it->second.containsValueOfType<std::uint32_t>()) {
          report(it->second.get<std::uint32_t>());
        }
      });
  const std::weak_ptr<int> alive = m_lifetimeToken;
  m_proxy->callMethodAsync("ClaimPresence")
      .onInterface(kInterface)
      .uponReplyInvoke([this, alive](std::optional<sdbus::Error> err) {
        if (alive.expired()) {
          return;
        }
        if (err.has_value()) {
          kLog.info("presence sensing unavailable: {}", err->what());
          return;
        }
        m_proxy->getPropertyAsync("Presence")
            .onInterface(kInterface)
            .uponReplyInvoke([this, alive](std::optional<sdbus::Error> getErr, sdbus::Variant value) {
              if (!alive.expired() && !getErr.has_value() && value.containsValueOfType<std::uint32_t>()) {
                report(value.get<std::uint32_t>());
              }
            });
      });
}

PresenceService::~PresenceService() {
  m_lifetimeToken.reset();
  try {
    m_proxy->callMethod("ReleasePresence").onInterface(kInterface).dontExpectReply();
  } catch (const sdbus::Error& e) {
    kLog.warn("presence release: {}", e.what());
  }
}

void PresenceService::report(std::uint32_t value) {
  m_onChange(value > 2 ? Presence::Unknown : static_cast<Presence>(value));
}
