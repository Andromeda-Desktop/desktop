#pragma once

#include <ghostty/vt.h>

#include <cstdint>

namespace term {

  // The key for the encoder: from the keysym and the layout's unshifted codepoint first, since virtual keyboards
  // (wtype, on-screen, remote input) send their own keymap and their evdev codes mean nothing; evdev last.
  GhosttyKey ghosttyKey(std::uint32_t evdev, std::uint32_t keysym, std::uint32_t unshifted);

} // namespace term
