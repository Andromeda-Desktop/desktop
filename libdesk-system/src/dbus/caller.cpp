#include "dbus/caller.h"

#include "core/log.h"
#include "protocols/bus_names.h"

#include <sdbus-c++/sdbus-c++.h>
#include <string>

namespace {
  constexpr Logger kLog("bus-caller");
} // namespace

bool isNameOwner(sdbus::IConnection& bus, std::string_view name, std::string_view sender) {
  if (sender.empty()) {
    return false;
  }
  try {
    auto daemon =
        sdbus::createProxy(bus, sdbus::ServiceName{busname::kDaemon}, sdbus::ObjectPath{busname::kDaemonPath});
    std::string owner;
    daemon->callMethod("GetNameOwner")
        .onInterface(busname::kDaemon)
        .withArguments(std::string{name})
        .storeResultsTo(owner);
    return owner == sender;
  } catch (const sdbus::Error& e) {
    kLog.debug("GetNameOwner({}) failed: {}", name, e.what());
    return false;
  }
}

void requirePortalFrontend(sdbus::IConnection& bus) {
  const char* sender = bus.getCurrentlyProcessedMessage().getSender();
  if (!isNameOwner(bus, busname::kPortalFrontend, std::string{sender != nullptr ? sender : ""})) {
    throw sdbus::Error(
        sdbus::Error::Name{"org.freedesktop.DBus.Error.AccessDenied"}, "Only the portal frontend may call"
    );
  }
}
