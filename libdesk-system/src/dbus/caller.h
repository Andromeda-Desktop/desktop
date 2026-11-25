#pragma once

#include <string_view>

namespace sdbus {
  class IConnection;
}

// The one owner of D-Bus caller identity (AR8): handlers ask here who sent the message being processed.

// True when `sender` (a unique name) owns `name` now. Blocks for one bus round trip.
[[nodiscard]] bool isNameOwner(sdbus::IConnection& bus, std::string_view name, std::string_view sender);

// Throws org.freedesktop.DBus.Error.AccessDenied unless the message `bus` is processing comes from the
// xdg-desktop-portal frontend. Every org.freedesktop.impl.portal.* method calls it first, except: high-rate session
// methods (Notify*, Clipboard) check the session owner, which only the frontend can be; Settings Read/ReadAll serve
// public values to apps that run without a frontend.
void requirePortalFrontend(sdbus::IConnection& bus);
