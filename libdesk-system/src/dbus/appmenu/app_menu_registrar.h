#pragma once

#include "dbus/name_owner_watch.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

class SessionBus;

namespace sdbus {
  class IObject;
  class IProxy;
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

  // The (service, menu object path) an X11 window registered.
  [[nodiscard]] std::optional<std::pair<std::string, std::string>> menuForWindow(std::uint32_t windowId) const;
  // Runs after a window's menu is registered or removed.
  void setChangedCallback(std::function<void()> callback) { m_changed = std::move(callback); }

private:
  SessionBus& m_bus;
  std::unique_ptr<sdbus::IObject> m_object;
  NameOwnerWatch m_ownerWatch;
  // X11 window id -> (service, menu object path).
  std::map<std::uint32_t, std::pair<std::string, std::string>> m_menus;
  std::function<void()> m_changed;

  void notifyChanged() const;
};
