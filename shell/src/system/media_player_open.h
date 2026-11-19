#pragma once

#include <string>

class CompositorPlatform;
class MprisService;

namespace media_player_open {

  // Brings the player forward: its own MPRIS Raise, else its window, else a launch of its desktop entry.
  void openPlayer(MprisService& mpris, CompositorPlatform* platform, const std::string& busName, const std::string& desktopEntry);

} // namespace media_player_open
