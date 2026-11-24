#include "dbus/appmenu/app_menu_registrar.h"

#include "core/log.h"
#include "dbus/session_bus.h"

#include <sdbus-c++/sdbus-c++.h>
#include <tuple>
#include <vector>

namespace {

  constexpr Logger kLog("appmenu-registrar");

  const sdbus::ServiceName kBusName{"com.canonical.AppMenu.Registrar"};
  const sdbus::ObjectPath kObjectPath{"/com/canonical/AppMenu/Registrar"};
  constexpr auto kInterface = "com.canonical.AppMenu.Registrar";

} // namespace

AppMenuRegistrar::AppMenuRegistrar(SessionBus& bus) : m_bus(bus) {
  m_object = sdbus::createObject(m_bus.connection(), kObjectPath);
  m_object
      ->addVTable(
          sdbus::registerMethod("RegisterWindow")
              .withInputParamNames("windowId", "menuObjectPath")
              .implementedAs([this](std::uint32_t windowId, const sdbus::ObjectPath& path) {
                const std::string sender = m_object->getCurrentlyProcessedMessage().getSender();
                m_menus[windowId] = {sender, path};
                m_object->emitSignal("WindowRegistered")
                    .onInterface(kInterface)
                    .withArguments(windowId, sdbus::BusName{sender}, path);
                notifyChanged();
              }),
          sdbus::registerMethod("UnregisterWindow")
              .withInputParamNames("windowId")
              .implementedAs([this](std::uint32_t windowId) {
                if (m_menus.erase(windowId) != 0) {
                  m_object->emitSignal("WindowUnregistered").onInterface(kInterface).withArguments(windowId);
                  notifyChanged();
                }
              }),
          sdbus::registerMethod("GetMenuForWindow")
              .withInputParamNames("windowId")
              .withOutputParamNames("service", "menuObjectPath")
              .implementedAs([this](std::uint32_t windowId) {
                const auto it = m_menus.find(windowId);
                if (it == m_menus.end()) {
                  throw sdbus::Error(sdbus::Error::Name{"com.canonical.AppMenu.Registrar.Error"}, "Window not found");
                }
                return std::tuple{sdbus::BusName{it->second.first}, sdbus::ObjectPath{it->second.second}};
              }),
          sdbus::registerMethod("GetMenus").withOutputParamNames("menus").implementedAs([this] {
            std::vector<sdbus::Struct<std::uint32_t, sdbus::BusName, sdbus::ObjectPath>> menus;
            for (const auto& [windowId, menu] : m_menus) {
              menus.emplace_back(windowId, sdbus::BusName{menu.first}, sdbus::ObjectPath{menu.second});
            }
            return menus;
          }),
          sdbus::registerSignal("WindowRegistered")
              .withParameters<std::uint32_t, sdbus::BusName, sdbus::ObjectPath>("windowId", "service", "menuObjectPath"),
          sdbus::registerSignal("WindowUnregistered").withParameters<std::uint32_t>("windowId")
      )
      .forInterface(kInterface);
  // A client that exits without UnregisterWindow leaves no menu behind.
  m_ownerWatch = NameOwnerWatch::all(
      m_bus.connection(),
      [this](const std::string& name, const std::string& /*oldOwner*/, const std::string& newOwner) {
        if (!newOwner.empty() || !name.starts_with(':')) {
          return;
        }
        const auto removed = std::erase_if(m_menus, [&](const auto& entry) {
          if (entry.second.first != name) {
            return false;
          }
          m_object->emitSignal("WindowUnregistered").onInterface(kInterface).withArguments(entry.first);
          return true;
        });
        if (removed != 0) {
          notifyChanged();
        }
      }
  );
  m_bus.connection().requestName(kBusName);
  kLog.info("serving {}", static_cast<const std::string&>(kBusName));
}

AppMenuRegistrar::~AppMenuRegistrar() {
  try {
    m_bus.connection().releaseName(kBusName);
  } catch (const sdbus::Error& e) {
    kLog.debug("release {} failed: {}", static_cast<const std::string&>(kBusName), e.what());
  }
}

std::optional<std::pair<std::string, std::string>> AppMenuRegistrar::menuForWindow(std::uint32_t windowId) const {
  const auto it = m_menus.find(windowId);
  if (it == m_menus.end()) {
    return std::nullopt;
  }
  return it->second;
}

void AppMenuRegistrar::notifyChanged() const {
  if (m_changed) {
    m_changed();
  }
}
