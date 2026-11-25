#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sdbus {
  class IConnection;
}

// The one owner of D-Bus caller identity (AR8): handlers ask here who sent the message being processed.

// Who sent a bus message. A Flatpak app is known by its app id. Any other process of the bus's user is the host,
// which can already act as that user.
struct BusCaller {
  enum class Kind : std::uint8_t { Host, Sandboxed };
  Kind kind = Kind::Host;
  std::string appId; // Sandboxed only: [Application] name from the app's /.flatpak-info.
  bool operator==(const BusCaller&) const = default;
};

// The identity of unique name `sender`; nullopt when the bus gives no pidfd, the process root cannot be read, or the
// process exits during the lookup. Blocks for one bus round trip per new sender; a unique name keeps its identity.
[[nodiscard]] std::optional<BusCaller> busCaller(sdbus::IConnection& bus, std::string_view sender);

// True when `sender` (a unique name) owns `name` now. Blocks for one bus round trip.
[[nodiscard]] bool isNameOwner(sdbus::IConnection& bus, std::string_view name, std::string_view sender);

// Throws org.freedesktop.DBus.Error.AccessDenied unless the message `bus` is processing comes from the owner of
// `name`: a daemon that calls back into our agent objects (NetworkManager, iwd).
void requireNameOwner(sdbus::IConnection& bus, std::string_view name);

// requireNameOwner for the xdg-desktop-portal frontend. Every org.freedesktop.impl.portal.* method calls it first,
// except: high-rate session methods (Notify*, Clipboard) check the session owner, which only the frontend can be;
// Settings Read/ReadAll serve public values to apps that run without a frontend.
void requirePortalFrontend(sdbus::IConnection& bus);
