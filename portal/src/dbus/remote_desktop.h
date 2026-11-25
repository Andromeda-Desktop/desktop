#pragma once

#include <memory>

namespace sdbus {
  class IConnection;
  class IObject;
} // namespace sdbus

class PollLoop;

namespace xdpu {

  class ScreenCastPortal;
  class SessionTable;
  class WaylandContext;

  // org.freedesktop.impl.portal.RemoteDesktop: a D-Bus bridge to andromeda_remote_input_v1. The compositor asks the
  // user, stores grants and owns the EIS server; this side only forwards.
  class RemoteDesktopPortal {
  public:
    RemoteDesktopPortal(
        PollLoop& loop, sdbus::IConnection& connection, sdbus::IObject& object, SessionTable& sessions,
        ScreenCastPortal& screencast, WaylandContext& wayland
    );
    ~RemoteDesktopPortal();

    RemoteDesktopPortal(const RemoteDesktopPortal&) = delete;
    RemoteDesktopPortal& operator=(const RemoteDesktopPortal&) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace xdpu
