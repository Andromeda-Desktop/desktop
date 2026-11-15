#pragma once

#include <cstdint>
#include <optional>
#include <xkbcommon/xkbcommon.h>

namespace input {

  // Latin letter for this key across layouts (e.g. Russian ф → a). nullopt if none.
  [[nodiscard]] inline std::optional<std::uint32_t>
  latinShortcutKeysym(xkb_keymap* keymap, xkb_keycode_t keycode) noexcept {
    if (keymap == nullptr) {
      return std::nullopt;
    }

    const xkb_layout_index_t layoutCount = xkb_keymap_num_layouts_for_key(keymap, keycode);
    for (xkb_layout_index_t layout = 0; layout < layoutCount; ++layout) {
      const xkb_keysym_t* syms = nullptr;
      const int count = xkb_keymap_key_get_syms_by_level(keymap, keycode, layout, 0, &syms);
      if (count < 1 || syms == nullptr) {
        continue;
      }
      const xkb_keysym_t lower = xkb_keysym_to_lower(syms[0]);
      if (lower >= XKB_KEY_a && lower <= XKB_KEY_z) {
        return static_cast<std::uint32_t>(lower);
      }
    }
    return std::nullopt;
  }

  // The key's codepoint with no modifier in the active layout (shift level 1); 0 for a key with no text.
  [[nodiscard]] inline std::uint32_t
  unshiftedCodepoint(xkb_keymap* keymap, xkb_state* state, xkb_keycode_t keycode) noexcept {
    if (keymap == nullptr || state == nullptr) {
      return 0;
    }
    const xkb_keysym_t* syms = nullptr;
    const xkb_layout_index_t layout = xkb_state_key_get_layout(state, keycode);
    if (layout == XKB_LAYOUT_INVALID || xkb_keymap_key_get_syms_by_level(keymap, keycode, layout, 0, &syms) < 1) {
      return 0;
    }
    return xkb_keysym_to_utf32(syms[0]);
  }

} // namespace input
