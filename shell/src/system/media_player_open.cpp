#include "system/media_player_open.h"

#include "compositors/compositor_platform.h"
#include "dbus/mpris/mpris_service.h"
#include "system/app_identity.h"
#include "system/desktop_entry.h"
#include "system/desktop_entry_launch.h"

#include <algorithm>
#include <cctype>

namespace media_player_open {

  void openPlayer(MprisService& mpris, CompositorPlatform* platform, const std::string& busName, const std::string& desktopEntry) {
    if (mpris.raise(busName) || desktopEntry.empty()) {
      return;
    }
    std::string idLower = desktopEntry;
    std::ranges::transform(idLower, idLower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (platform != nullptr && !platform->windowsForApp(idLower, idLower).empty()) {
      platform->activateToplevelForAppId(desktopEntry);
    } else if (const auto entry = app_identity::findDesktopEntry(desktopEntry, desktopEntries()); entry.has_value()) {
      (void)desktop_entry_launch::launchEntry(*entry, {.dbusActivatable = entry->dbusActivatable, .dbusAppId = entry->id});
    }
  }

} // namespace media_player_open
