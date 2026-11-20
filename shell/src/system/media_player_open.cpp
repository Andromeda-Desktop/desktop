#include "system/media_player_open.h"

#include "compositors/compositor_platform.h"
#include "dbus/mpris/mpris_service.h"
#include "system/app_identity.h"
#include "system/desktop_entry.h"
#include "system/desktop_entry_launch.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace media_player_open {

  namespace {

    // org.mpris.MediaPlayer2.vivaldi.instance1935 -> vivaldi
    std::string busNameApp(std::string_view busName) {
      constexpr std::string_view kPrefix = "org.mpris.MediaPlayer2.";
      if (!busName.starts_with(kPrefix)) {
        return {};
      }
      busName.remove_prefix(kPrefix.size());
      return std::string(busName.substr(0, busName.find('.')));
    }

    std::string lower(std::string value) {
      std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return value;
    }

  } // namespace

  void openPlayer(MprisService& mpris, CompositorPlatform* platform, const std::string& busName, const std::string& desktopEntry) {
    if (mpris.raise(busName)) {
      return;
    }
    // Chromium browsers send no DesktopEntry; their bus name holds the app name.
    const std::string hint = desktopEntry.empty() ? busNameApp(busName) : desktopEntry;
    if (hint.empty()) {
      return;
    }
    const auto entry = app_identity::findDesktopEntry(hint, desktopEntries());
    const std::string id = lower(entry.has_value() ? entry->id : hint);
    const std::string wmClass = lower(entry.has_value() && !entry->startupWmClass.empty() ? entry->startupWmClass : id);
    if (platform != nullptr) {
      if (const auto windows = platform->windowsForApp(id, wmClass); !windows.empty()) {
        platform->activateToplevelInfo(windows.back());
        return;
      }
    }
    if (entry.has_value()) {
      (void)desktop_entry_launch::launchEntry(*entry, {.dbusActivatable = entry->dbusActivatable, .dbusAppId = entry->id});
    }
  }

} // namespace media_player_open
