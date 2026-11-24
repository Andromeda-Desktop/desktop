#pragma once

// D-Bus names every binary shares (C13): the compositor, libdesk-system, the shell and apps compile this header.
namespace busname {

  // The system daemon (system/crates/andromeda-system).
  inline constexpr const char* kSystem1 = "org.andromeda.System1";
  // The bus daemon itself: NameHasOwner, NameOwnerChanged, ListNames.
  inline constexpr const char* kDaemon = "org.freedesktop.DBus";
  inline constexpr const char* kDaemonPath = "/org/freedesktop/DBus";

} // namespace busname
