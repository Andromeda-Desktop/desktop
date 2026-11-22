#pragma once

#include "shell/bar/widgets/dbus_menu_client.h"

#include <cstdint>
#include <string>

namespace compositors::andromeda {
  class AndromedaRuntime;
} // namespace compositors::andromeda

// A libdesk app's dsk_app_menu_v1 tree, read once from the compositor (IPC `app-menu`). A new tree has a new serial and
// gets a new client. Activations name this window and serial, so they never reach another window or a newer tree.
class DskMenuClient : public AppMenuSource {
public:
  DskMenuClient(const compositors::andromeda::AndromedaRuntime& runtime, std::string windowId, std::uint64_t serial);

  [[nodiscard]] const DbusMenuNode& root() const noexcept override { return m_root; }
  [[nodiscard]] bool loaded() const noexcept override { return m_loaded; }
  void activate(std::int32_t id) override;

private:
  const compositors::andromeda::AndromedaRuntime& m_runtime;
  std::string m_windowId;
  std::uint64_t m_serial = 0;
  DbusMenuNode m_root;
  bool m_loaded = false;
};
