#pragma once

#include <cstdint>

namespace andromeda {

  // What the user can see of a mapped window. Off: the scene draws it, but the session is locked or every output it is
  // on is powered off. Hidden: the scene does not draw it (minimized, another workspace, a background tab).
  // Occluded is reserved for opaque coverage, which nothing computes yet.
  enum class Presence : uint8_t { Shown, Occluded, Hidden, Off };

  struct PresenceInputs {
    bool presented = false;
    bool sessionLocked = false;
    bool outputsOff = false;
    // A capture or an overview card draws it off the scene.
    bool drawnOffscreen = false;
    // A game or a background_render rule keeps its frame loop alive, even behind the lock.
    bool backgroundRender = false;
  };

  struct PresenceFacts {
    Presence presence = Presence::Hidden;
    // The window keeps frame callbacks and is not suspended.
    bool drawsFrames = false;

    constexpr bool operator==(const PresenceFacts&) const = default;
  };

  // The one rule every visibility export derives from. The lock wins over off-scene drawing, so nothing renders a
  // window for a capture behind the lock screen; a background_render window only gets callbacks, so it keeps them.
  constexpr PresenceFacts presenceOf(const PresenceInputs& in) {
    Presence presence = Presence::Shown;
    if (!in.presented) {
      presence = Presence::Hidden;
    } else if (in.sessionLocked || in.outputsOff) {
      presence = Presence::Off;
    }
    const bool drawsFrames =
        presence == Presence::Shown || in.backgroundRender || (in.drawnOffscreen && !in.sessionLocked);
    return {.presence = presence, .drawsFrames = drawsFrames};
  }

} // namespace andromeda
