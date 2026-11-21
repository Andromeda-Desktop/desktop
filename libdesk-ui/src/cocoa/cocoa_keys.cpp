#include "cocoa/cocoa_keys.h"

#include "core/input/input_codes.h"

#include <array>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace cocoa_keys {
  namespace {
    struct KeyEntry {
      std::uint16_t evdev = 0;
      std::uint32_t keysym = 0;
    };

    // Indexed by kVK_* (HIToolbox Events.h); keysym 0 = the keysym comes from the text the key types.
    constexpr std::array<KeyEntry, 0x80> kKeys = [] {
      std::array<KeyEntry, 0x80> t{};
      auto set = [&t](std::uint16_t mac, std::uint16_t evdev, std::uint32_t keysym = 0) { t[mac] = {evdev, keysym}; };
      set(0x00, KEY_A);
      set(0x01, KEY_S);
      set(0x02, KEY_D);
      set(0x03, KEY_F);
      set(0x04, KEY_H);
      set(0x05, KEY_G);
      set(0x06, KEY_Z);
      set(0x07, KEY_X);
      set(0x08, KEY_C);
      set(0x09, KEY_V);
      set(0x0A, KEY_102ND);
      set(0x0B, KEY_B);
      set(0x0C, KEY_Q);
      set(0x0D, KEY_W);
      set(0x0E, KEY_E);
      set(0x0F, KEY_R);
      set(0x10, KEY_Y);
      set(0x11, KEY_T);
      set(0x12, KEY_1);
      set(0x13, KEY_2);
      set(0x14, KEY_3);
      set(0x15, KEY_4);
      set(0x16, KEY_6);
      set(0x17, KEY_5);
      set(0x18, KEY_EQUAL);
      set(0x19, KEY_9);
      set(0x1A, KEY_7);
      set(0x1B, KEY_MINUS);
      set(0x1C, KEY_8);
      set(0x1D, KEY_0);
      set(0x1E, KEY_RIGHTBRACE);
      set(0x1F, KEY_O);
      set(0x20, KEY_U);
      set(0x21, KEY_LEFTBRACE);
      set(0x22, KEY_I);
      set(0x23, KEY_P);
      set(0x24, KEY_ENTER, XKB_KEY_Return);
      set(0x25, KEY_L);
      set(0x26, KEY_J);
      set(0x27, KEY_APOSTROPHE);
      set(0x28, KEY_K);
      set(0x29, KEY_SEMICOLON);
      set(0x2A, KEY_BACKSLASH);
      set(0x2B, KEY_COMMA);
      set(0x2C, KEY_SLASH);
      set(0x2D, KEY_N);
      set(0x2E, KEY_M);
      set(0x2F, KEY_DOT);
      set(0x30, KEY_TAB, XKB_KEY_Tab);
      set(0x31, KEY_SPACE, XKB_KEY_space);
      set(0x32, KEY_GRAVE);
      set(0x33, KEY_BACKSPACE, XKB_KEY_BackSpace);
      set(0x35, KEY_ESC, XKB_KEY_Escape);
      set(0x36, KEY_RIGHTMETA, XKB_KEY_Super_R);
      set(0x37, KEY_LEFTMETA, XKB_KEY_Super_L);
      set(0x38, KEY_LEFTSHIFT, XKB_KEY_Shift_L);
      set(0x39, KEY_CAPSLOCK, XKB_KEY_Caps_Lock);
      set(0x3A, KEY_LEFTALT, XKB_KEY_Alt_L);
      set(0x3B, KEY_LEFTCTRL, XKB_KEY_Control_L);
      set(0x3C, KEY_RIGHTSHIFT, XKB_KEY_Shift_R);
      set(0x3D, KEY_RIGHTALT, XKB_KEY_Alt_R);
      set(0x3E, KEY_RIGHTCTRL, XKB_KEY_Control_R);
      set(0x3F, KEY_FN);
      set(0x40, KEY_F17, XKB_KEY_F17);
      set(0x41, KEY_KPDOT, XKB_KEY_KP_Decimal);
      set(0x43, KEY_KPASTERISK, XKB_KEY_KP_Multiply);
      set(0x45, KEY_KPPLUS, XKB_KEY_KP_Add);
      set(0x47, KEY_NUMLOCK, XKB_KEY_Num_Lock);
      set(0x48, KEY_VOLUMEUP, XKB_KEY_XF86AudioRaiseVolume);
      set(0x49, KEY_VOLUMEDOWN, XKB_KEY_XF86AudioLowerVolume);
      set(0x4A, KEY_MUTE, XKB_KEY_XF86AudioMute);
      set(0x4B, KEY_KPSLASH, XKB_KEY_KP_Divide);
      set(0x4C, KEY_KPENTER, XKB_KEY_KP_Enter);
      set(0x4E, KEY_KPMINUS, XKB_KEY_KP_Subtract);
      set(0x4F, KEY_F18, XKB_KEY_F18);
      set(0x50, KEY_F19, XKB_KEY_F19);
      set(0x51, KEY_KPEQUAL, XKB_KEY_KP_Equal);
      set(0x52, KEY_KP0, XKB_KEY_KP_0);
      set(0x53, KEY_KP1, XKB_KEY_KP_1);
      set(0x54, KEY_KP2, XKB_KEY_KP_2);
      set(0x55, KEY_KP3, XKB_KEY_KP_3);
      set(0x56, KEY_KP4, XKB_KEY_KP_4);
      set(0x57, KEY_KP5, XKB_KEY_KP_5);
      set(0x58, KEY_KP6, XKB_KEY_KP_6);
      set(0x59, KEY_KP7, XKB_KEY_KP_7);
      set(0x5A, KEY_F20, XKB_KEY_F20);
      set(0x5B, KEY_KP8, XKB_KEY_KP_8);
      set(0x5C, KEY_KP9, XKB_KEY_KP_9);
      set(0x5D, KEY_YEN);
      set(0x5E, KEY_RO);
      set(0x5F, KEY_KPCOMMA, XKB_KEY_KP_Separator);
      set(0x60, KEY_F5, XKB_KEY_F5);
      set(0x61, KEY_F6, XKB_KEY_F6);
      set(0x62, KEY_F7, XKB_KEY_F7);
      set(0x63, KEY_F3, XKB_KEY_F3);
      set(0x64, KEY_F8, XKB_KEY_F8);
      set(0x65, KEY_F9, XKB_KEY_F9);
      set(0x66, KEY_MUHENKAN, XKB_KEY_Muhenkan);
      set(0x67, KEY_F11, XKB_KEY_F11);
      set(0x68, KEY_HENKAN, XKB_KEY_Henkan);
      set(0x69, KEY_F13, XKB_KEY_F13);
      set(0x6A, KEY_F16, XKB_KEY_F16);
      set(0x6B, KEY_F14, XKB_KEY_F14);
      set(0x6D, KEY_F10, XKB_KEY_F10);
      set(0x6E, KEY_COMPOSE, XKB_KEY_Menu);
      set(0x6F, KEY_F12, XKB_KEY_F12);
      set(0x71, KEY_F15, XKB_KEY_F15);
      set(0x72, KEY_INSERT, XKB_KEY_Insert);
      set(0x73, KEY_HOME, XKB_KEY_Home);
      set(0x74, KEY_PAGEUP, XKB_KEY_Prior);
      set(0x75, KEY_DELETE, XKB_KEY_Delete);
      set(0x76, KEY_F4, XKB_KEY_F4);
      set(0x77, KEY_END, XKB_KEY_End);
      set(0x78, KEY_F2, XKB_KEY_F2);
      set(0x79, KEY_PAGEDOWN, XKB_KEY_Next);
      set(0x7A, KEY_F1, XKB_KEY_F1);
      set(0x7B, KEY_LEFT, XKB_KEY_Left);
      set(0x7C, KEY_RIGHT, XKB_KEY_Right);
      set(0x7D, KEY_DOWN, XKB_KEY_Down);
      set(0x7E, KEY_UP, XKB_KEY_Up);
      return t;
    }();
  } // namespace

  std::uint32_t evdevCode(std::uint16_t macKeyCode) noexcept {
    return macKeyCode < kKeys.size() ? kKeys[macKeyCode].evdev : 0;
  }

  std::uint32_t functionKeysym(std::uint16_t macKeyCode, bool shift) noexcept {
    if (macKeyCode >= kKeys.size()) {
      return 0;
    }
    const std::uint32_t sym = kKeys[macKeyCode].keysym;
    return sym == XKB_KEY_Tab && shift ? XKB_KEY_ISO_Left_Tab : sym;
  }

  std::uint32_t keysymForCodepoint(std::uint32_t codepoint) noexcept {
    if ((codepoint >= 0x20 && codepoint <= 0x7E) || (codepoint >= 0xA0 && codepoint <= 0xFF)) {
      return codepoint;
    }
    return codepoint == 0 ? 0 : 0x01000000U | codepoint;
  }
} // namespace cocoa_keys
