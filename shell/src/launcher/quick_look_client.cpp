#include "launcher/quick_look_client.h"

#include "core/log.h"
#include "dbus/session_bus.h"
#include "net/uri.h"

#include <sdbus-c++/sdbus-c++.h>

namespace {
  constexpr Logger kLog("quick-look");
  const sdbus::InterfaceName kInterface{"org.gnome.NautilusPreviewer2"};
} // namespace

QuickLookClient::QuickLookClient(SessionBus& bus) : m_bus(bus) {}

QuickLookClient::~QuickLookClient() = default;

sdbus::IProxy& QuickLookClient::proxy() {
  if (!m_proxy) {
    m_proxy = sdbus::createProxy(
        m_bus.connection(), sdbus::ServiceName{"org.gnome.NautilusPreviewer"},
        sdbus::ObjectPath{"/org/gnome/NautilusPreviewer"}
    );
  }
  return *m_proxy;
}

void QuickLookClient::toggle(const std::string& path) {
  const std::string fileUri = uri::fileUri(path);
  // The launcher is a layer surface with no exported handle: the preview opens unparented.
  proxy()
      .callMethodAsync("ShowFile")
      .onInterface(kInterface)
      .withArguments(fileUri, std::string{}, true)
      .uponReplyInvoke([fileUri](std::optional<sdbus::Error> error) {
        if (error.has_value()) {
          kLog.warn("Quick Look for {} failed: {}", fileUri, error->what());
        }
      });
}

void QuickLookClient::close() {
  proxy().callMethodAsync("Close").onInterface(kInterface).uponReplyInvoke([](std::optional<sdbus::Error> error) {
    if (error.has_value()) {
      kLog.debug("Quick Look close failed: {}", error->what());
    }
  });
}
