#pragma once

#include "dbus/request.h"
#include "dbus/session.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sdbus {
  class IConnection;
  class IObject;
} // namespace sdbus

class PollLoop;

namespace xdpu {

  class PipeWireContext;
  class Session;
  class SessionTable;
  class WaylandContext;
  struct ScreenCastCommand;
  struct Config;

  // Sources from ScreenCast restore data ("andromeda", version, entries); empty when it is not valid.
  std::vector<Session::Selection> restoreSelections(const sdbus::Variant& data);

  using StartReply = std::move_only_function<void(uint32_t response, PortalResults results)>;

  class ScreenCastPortal {
  public:
    ScreenCastPortal(
        PollLoop& loop, sdbus::IConnection& connection, sdbus::IObject& object, SessionTable& sessions,
        const Config& config, WaylandContext& wayland, PipeWireContext& pipewire
    );
    ~ScreenCastPortal();

    ScreenCastPortal(const ScreenCastPortal&) = delete;
    ScreenCastPortal& operator=(const ScreenCastPortal&) = delete;
    ScreenCastPortal(ScreenCastPortal&&) = delete;
    ScreenCastPortal& operator=(ScreenCastPortal&&) = delete;

    void onConfigChanged(const Config& oldCfg, const Config& newCfg);
    void onScreenCastCommand(const ScreenCastCommand& command);
    void onFocusedWindowChanged(const std::optional<std::string>& identifier);
    void onFocusedOutputChanged(const std::optional<std::string>& output);
    void setActiveChangedHandler(std::function<void(bool)> handler);
    // Runs the source chooser and captures for a session started by another portal (RemoteDesktop.Start, which owns
    // the Request); `reply` gets the ScreenCast results. The returned function cancels a Start still running.
    std::function<void()> startSources(
        const std::shared_ptr<Session>& session, const std::string& parentWindow, StartReply reply
    );

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace xdpu
