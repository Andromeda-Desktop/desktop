#include "dbus/dbus.h"

#include "config/config.h"
#include "dbus/remote_desktop.h"
#include "dbus/session_bus.h"
#include "dbus/session_bus_poll_source.h"
#include "dbus/screencast.h"
#include "dbus/screenshot.h"
#include "dbus/session_table.h"
#include "core/poll_loop.h"
#include "pipewire/pipewire.h"
#include "andromeda/ipc.h"
#include "wayland/wayland.h"

#include <csignal>
#include <cstdio>
#include <sdbus-c++/sdbus-c++.h>

namespace xdpu {

  namespace {

    constexpr char kBusName[] = "org.freedesktop.impl.portal.desktop.andromeda";
    constexpr char kDesktopPath[] = "/org/freedesktop/portal/desktop";

  } // namespace

  struct DbusPortal::Impl {
    PollLoop& loop;
    Config config;
    WaylandContext& wayland;
    PipeWireContext& pipewire;
    SessionBus bus{sdbus::ServiceName{kBusName}};
    SessionBusPollSource busSource{bus};
    std::unique_ptr<sdbus::IObject> desktopObject;
    std::unique_ptr<SessionTable> sessions;
    std::unique_ptr<ScreenshotPortal> screenshot;
    std::unique_ptr<ScreenCastPortal> screencast;
    std::unique_ptr<RemoteDesktopPortal> remoteDesktop;

    Impl(PollLoop& loop, const Config& config, WaylandContext& wayland, PipeWireContext& pipewire)
        : loop(loop), config(config), wayland(wayland), pipewire(pipewire) {
      std::signal(SIGPIPE, SIG_IGN);
      sdbus::IConnection& connection = bus.connection();
      // A portal without its bus serves nothing; the Wayland and PipeWire fds would keep the loop alive forever.
      bus.setDisconnectedCallback([&loop] { loop.quit(); });
      connection.addMatch(
          "type='signal',interface='org.freedesktop.DBus.Local',member='Disconnected'",
          [&loop](sdbus::Message) { loop.quit(); }
      );
      desktopObject = sdbus::createObject(connection, sdbus::ObjectPath{kDesktopPath});

      screenshot = std::make_unique<ScreenshotPortal>(loop, connection, *desktopObject, this->config, wayland);
      sessions = std::make_unique<SessionTable>(connection);
      screencast = std::make_unique<ScreenCastPortal>(
          loop, connection, *desktopObject, *sessions, this->config, wayland, pipewire
      );
      remoteDesktop =
          std::make_unique<RemoteDesktopPortal>(loop, connection, *desktopObject, *sessions, *screencast, wayland);

      loop.addSource(&busSource);
    }

    ~Impl() {
      loop.removeSource(&busSource);
      remoteDesktop.reset();
      screencast.reset();
      sessions.reset();
      screenshot.reset();
      desktopObject.reset();
    }
  };

  DbusPortal::DbusPortal(PollLoop& loop, const Config& config, WaylandContext& wayland, PipeWireContext& pipewire)
      : m_impl(std::make_unique<Impl>(loop, config, wayland, pipewire)) {}

  DbusPortal::~DbusPortal() = default;

  void DbusPortal::onConfigChanged(const Config& oldCfg, const Config& newCfg) {
    m_impl->config = newCfg;
    if (m_impl->screenshot) {
      m_impl->screenshot->onConfigChanged(oldCfg, newCfg);
    }
    if (m_impl->screencast) {
      m_impl->screencast->onConfigChanged(oldCfg, newCfg);
    }
  }

  void DbusPortal::onScreenCastCommand(const ScreenCastCommand& command) {
    if (m_impl->screencast) {
      m_impl->screencast->onScreenCastCommand(command);
    }
  }

  void DbusPortal::onFocusedWindowChanged(const std::optional<std::string>& identifier) {
    if (m_impl->screencast) {
      m_impl->screencast->onFocusedWindowChanged(identifier);
    }
  }

  void DbusPortal::onFocusedOutputChanged(const std::optional<std::string>& output) {
    m_impl->wayland.setFocusedOutput(output);
    if (m_impl->screencast) {
      m_impl->screencast->onFocusedOutputChanged(output);
    }
  }

  void DbusPortal::setScreenCastActiveHandler(std::function<void(bool)> handler) {
    if (m_impl->screencast) {
      m_impl->screencast->setActiveChangedHandler(std::move(handler));
    }
  }

} // namespace xdpu
