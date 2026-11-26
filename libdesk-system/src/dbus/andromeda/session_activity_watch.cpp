#include "dbus/andromeda/session_activity_watch.h"

#include "core/log.h"
#include "dbus/session_bus.h"

#include <cstdlib>
#include <map>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>

namespace {
  constexpr Logger kLog("session-activity");
  const sdbus::ObjectPath kAppsPath{"/org/andromeda/Compositor1/Apps"};
  constexpr auto kAppsInterface = "org.andromeda.Compositor1.Apps";
  using VariantMap = std::map<std::string, sdbus::Variant>;
} // namespace

std::string compositorBusName() {
  const char* name = std::getenv("ANDROMEDA_COMPOSITOR_BUS_NAME");
  return name != nullptr && name[0] != '\0' ? name : "org.andromeda.Compositor1";
}

SessionActivityWatch::SessionActivityWatch(SessionBus& bus)
    : m_apps(sdbus::createProxy(bus.connection(), sdbus::ServiceName{compositorBusName()}, kAppsPath)) {
  m_apps->uponSignal("PropertiesChanged")
      .onInterface("org.freedesktop.DBus.Properties")
      .call([this](const std::string& iface, const VariantMap& changed, const std::vector<std::string>& /*invalidated*/) {
        if (iface != kAppsInterface) {
          return;
        }
        if (const auto it = changed.find("Activity"); it != changed.end()) {
          apply(it->second.get<std::string>());
        }
        if (const auto it = changed.find("Presenting"); it != changed.end()) {
          applyPresenting(it->second.get<bool>());
        }
      });
  const std::weak_ptr<int> alive = m_alive;
  m_apps->getPropertyAsync("Activity").onInterface(kAppsInterface).uponReplyInvoke(
      [this, alive](std::optional<sdbus::Error> error, const sdbus::Variant& value) {
        if (alive.expired()) {
          return;
        }
        if (error.has_value()) {
          kLog.warn("compositor session activity unavailable: {}", error->what());
          return;
        }
        apply(value.get<std::string>());
      }
  );
  m_apps->getPropertyAsync("Presenting").onInterface(kAppsInterface).uponReplyInvoke(
      [this, alive](std::optional<sdbus::Error> error, const sdbus::Variant& value) {
        if (!alive.expired() && !error.has_value()) {
          applyPresenting(value.get<bool>());
        }
      }
  );
}

void SessionActivityWatch::applyPresenting(bool presenting) {
  if (m_presenting == presenting) {
    return;
  }
  m_presenting = presenting;
  for (const ChangeListener& listener : m_changeListeners) {
    listener();
  }
}

SessionActivityWatch::~SessionActivityWatch() = default;

void SessionActivityWatch::addSleepListener(SleepListener listener) { m_listeners.push_back(std::move(listener)); }

void SessionActivityWatch::apply(const std::string& activity) {
  if (const bool idle = activity == "idle"; idle != m_idle) {
    m_idle = idle;
    for (const SleepListener& listener : m_idleListeners) {
      listener(idle);
    }
  }
  const bool sleeping = activity == "sleeping" || activity == "sleeping-locked";
  if (sleeping == m_sleeping) {
    return;
  }
  m_sleeping = sleeping;
  kLog.info("session {}", sleeping ? "going to sleep" : "resumed");
  for (const SleepListener& listener : m_listeners) {
    listener(sleeping);
  }
}
