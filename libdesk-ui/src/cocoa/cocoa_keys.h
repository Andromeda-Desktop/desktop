#pragma once

#include <cstdint>

namespace cocoa_keys {
  // macOS virtual key code (kVK_*) to Linux evdev code; 0 for an unknown key.
  [[nodiscard]] std::uint32_t evdevCode(std::uint16_t macKeyCode) noexcept;
  // The keysym of a key whose keysym does not follow from its text (arrows, F keys, modifiers, keypad); 0 otherwise.
  [[nodiscard]] std::uint32_t functionKeysym(std::uint16_t macKeyCode, bool shift) noexcept;
  [[nodiscard]] std::uint32_t keysymForCodepoint(std::uint32_t codepoint) noexcept;
} // namespace cocoa_keys
