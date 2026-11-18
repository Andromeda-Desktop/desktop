#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>

class SessionBus;

namespace sdbus {
  class IObject;
}

// Owns com.canonical.AppMenu.Registrar. Chromium and Electron export their menu bar only while this name has an
// owner; on Wayland they then hand the address to the compositor through org_kde_kwin_appmenu. X11 clients register
// their window id here instead.
class AppMenuRegistrar {
public:
  explicit AppMenuRegistrar(SessionBus& bus);
  ~AppMenuRegistrar();

  AppMenuRegistrar(const AppMenuRegistrar&) = delete;
  AppMenuRegistrar& operator=(const AppMenuRegistrar&) = delete;

private:
  SessionBus& m_bus;
  std::unique_ptr<sdbus::IObject> m_object;
  // X11 window id -> (service, menu object path).
  std::map<std::uint32_t, std::pair<std::string, std::string>> m_menus;
};
