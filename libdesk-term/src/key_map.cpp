#include "key_map.h"

#include "core/input/input_codes.h"
#include <xkbcommon/xkbcommon-keysyms.h>

namespace term {

  namespace {

  GhosttyKey fromEvdev(std::uint32_t evdev) {
    switch (evdev) {
    case KEY_ESC: return GHOSTTY_KEY_ESCAPE;
    case KEY_1: return GHOSTTY_KEY_DIGIT_1;
    case KEY_2: return GHOSTTY_KEY_DIGIT_2;
    case KEY_3: return GHOSTTY_KEY_DIGIT_3;
    case KEY_4: return GHOSTTY_KEY_DIGIT_4;
    case KEY_5: return GHOSTTY_KEY_DIGIT_5;
    case KEY_6: return GHOSTTY_KEY_DIGIT_6;
    case KEY_7: return GHOSTTY_KEY_DIGIT_7;
    case KEY_8: return GHOSTTY_KEY_DIGIT_8;
    case KEY_9: return GHOSTTY_KEY_DIGIT_9;
    case KEY_0: return GHOSTTY_KEY_DIGIT_0;
    case KEY_MINUS: return GHOSTTY_KEY_MINUS;
    case KEY_EQUAL: return GHOSTTY_KEY_EQUAL;
    case KEY_BACKSPACE: return GHOSTTY_KEY_BACKSPACE;
    case KEY_TAB: return GHOSTTY_KEY_TAB;
    case KEY_Q: return GHOSTTY_KEY_Q;
    case KEY_W: return GHOSTTY_KEY_W;
    case KEY_E: return GHOSTTY_KEY_E;
    case KEY_R: return GHOSTTY_KEY_R;
    case KEY_T: return GHOSTTY_KEY_T;
    case KEY_Y: return GHOSTTY_KEY_Y;
    case KEY_U: return GHOSTTY_KEY_U;
    case KEY_I: return GHOSTTY_KEY_I;
    case KEY_O: return GHOSTTY_KEY_O;
    case KEY_P: return GHOSTTY_KEY_P;
    case KEY_LEFTBRACE: return GHOSTTY_KEY_BRACKET_LEFT;
    case KEY_RIGHTBRACE: return GHOSTTY_KEY_BRACKET_RIGHT;
    case KEY_ENTER: return GHOSTTY_KEY_ENTER;
    case KEY_LEFTCTRL: return GHOSTTY_KEY_CONTROL_LEFT;
    case KEY_A: return GHOSTTY_KEY_A;
    case KEY_S: return GHOSTTY_KEY_S;
    case KEY_D: return GHOSTTY_KEY_D;
    case KEY_F: return GHOSTTY_KEY_F;
    case KEY_G: return GHOSTTY_KEY_G;
    case KEY_H: return GHOSTTY_KEY_H;
    case KEY_J: return GHOSTTY_KEY_J;
    case KEY_K: return GHOSTTY_KEY_K;
    case KEY_L: return GHOSTTY_KEY_L;
    case KEY_SEMICOLON: return GHOSTTY_KEY_SEMICOLON;
    case KEY_APOSTROPHE: return GHOSTTY_KEY_QUOTE;
    case KEY_GRAVE: return GHOSTTY_KEY_BACKQUOTE;
    case KEY_LEFTSHIFT: return GHOSTTY_KEY_SHIFT_LEFT;
    case KEY_BACKSLASH: return GHOSTTY_KEY_BACKSLASH;
    case KEY_Z: return GHOSTTY_KEY_Z;
    case KEY_X: return GHOSTTY_KEY_X;
    case KEY_C: return GHOSTTY_KEY_C;
    case KEY_V: return GHOSTTY_KEY_V;
    case KEY_B: return GHOSTTY_KEY_B;
    case KEY_N: return GHOSTTY_KEY_N;
    case KEY_M: return GHOSTTY_KEY_M;
    case KEY_COMMA: return GHOSTTY_KEY_COMMA;
    case KEY_DOT: return GHOSTTY_KEY_PERIOD;
    case KEY_SLASH: return GHOSTTY_KEY_SLASH;
    case KEY_RIGHTSHIFT: return GHOSTTY_KEY_SHIFT_RIGHT;
    case KEY_KPASTERISK: return GHOSTTY_KEY_NUMPAD_MULTIPLY;
    case KEY_LEFTALT: return GHOSTTY_KEY_ALT_LEFT;
    case KEY_SPACE: return GHOSTTY_KEY_SPACE;
    case KEY_CAPSLOCK: return GHOSTTY_KEY_CAPS_LOCK;
    case KEY_F1: return GHOSTTY_KEY_F1;
    case KEY_F2: return GHOSTTY_KEY_F2;
    case KEY_F3: return GHOSTTY_KEY_F3;
    case KEY_F4: return GHOSTTY_KEY_F4;
    case KEY_F5: return GHOSTTY_KEY_F5;
    case KEY_F6: return GHOSTTY_KEY_F6;
    case KEY_F7: return GHOSTTY_KEY_F7;
    case KEY_F8: return GHOSTTY_KEY_F8;
    case KEY_F9: return GHOSTTY_KEY_F9;
    case KEY_F10: return GHOSTTY_KEY_F10;
    case KEY_NUMLOCK: return GHOSTTY_KEY_NUM_LOCK;
    case KEY_SCROLLLOCK: return GHOSTTY_KEY_SCROLL_LOCK;
    case KEY_KP7: return GHOSTTY_KEY_NUMPAD_7;
    case KEY_KP8: return GHOSTTY_KEY_NUMPAD_8;
    case KEY_KP9: return GHOSTTY_KEY_NUMPAD_9;
    case KEY_KPMINUS: return GHOSTTY_KEY_NUMPAD_SUBTRACT;
    case KEY_KP4: return GHOSTTY_KEY_NUMPAD_4;
    case KEY_KP5: return GHOSTTY_KEY_NUMPAD_5;
    case KEY_KP6: return GHOSTTY_KEY_NUMPAD_6;
    case KEY_KPPLUS: return GHOSTTY_KEY_NUMPAD_ADD;
    case KEY_KP1: return GHOSTTY_KEY_NUMPAD_1;
    case KEY_KP2: return GHOSTTY_KEY_NUMPAD_2;
    case KEY_KP3: return GHOSTTY_KEY_NUMPAD_3;
    case KEY_KP0: return GHOSTTY_KEY_NUMPAD_0;
    case KEY_KPDOT: return GHOSTTY_KEY_NUMPAD_DECIMAL;
    case KEY_102ND: return GHOSTTY_KEY_INTL_BACKSLASH;
    case KEY_F11: return GHOSTTY_KEY_F11;
    case KEY_F12: return GHOSTTY_KEY_F12;
    case KEY_RO: return GHOSTTY_KEY_INTL_RO;
    case KEY_KPENTER: return GHOSTTY_KEY_NUMPAD_ENTER;
    case KEY_RIGHTCTRL: return GHOSTTY_KEY_CONTROL_RIGHT;
    case KEY_KPSLASH: return GHOSTTY_KEY_NUMPAD_DIVIDE;
    case KEY_SYSRQ: return GHOSTTY_KEY_PRINT_SCREEN;
    case KEY_RIGHTALT: return GHOSTTY_KEY_ALT_RIGHT;
    case KEY_HOME: return GHOSTTY_KEY_HOME;
    case KEY_UP: return GHOSTTY_KEY_ARROW_UP;
    case KEY_PAGEUP: return GHOSTTY_KEY_PAGE_UP;
    case KEY_LEFT: return GHOSTTY_KEY_ARROW_LEFT;
    case KEY_RIGHT: return GHOSTTY_KEY_ARROW_RIGHT;
    case KEY_END: return GHOSTTY_KEY_END;
    case KEY_DOWN: return GHOSTTY_KEY_ARROW_DOWN;
    case KEY_PAGEDOWN: return GHOSTTY_KEY_PAGE_DOWN;
    case KEY_INSERT: return GHOSTTY_KEY_INSERT;
    case KEY_DELETE: return GHOSTTY_KEY_DELETE;
    case KEY_KPEQUAL: return GHOSTTY_KEY_NUMPAD_EQUAL;
    case KEY_PAUSE: return GHOSTTY_KEY_PAUSE;
    case KEY_KPCOMMA: return GHOSTTY_KEY_NUMPAD_COMMA;
    case KEY_YEN: return GHOSTTY_KEY_INTL_YEN;
    case KEY_LEFTMETA: return GHOSTTY_KEY_META_LEFT;
    case KEY_RIGHTMETA: return GHOSTTY_KEY_META_RIGHT;
    case KEY_COMPOSE: return GHOSTTY_KEY_CONTEXT_MENU;
    case KEY_F13: return GHOSTTY_KEY_F13;
    case KEY_F14: return GHOSTTY_KEY_F14;
    case KEY_F15: return GHOSTTY_KEY_F15;
    case KEY_F16: return GHOSTTY_KEY_F16;
    case KEY_F17: return GHOSTTY_KEY_F17;
    case KEY_F18: return GHOSTTY_KEY_F18;
    case KEY_F19: return GHOSTTY_KEY_F19;
    case KEY_F20: return GHOSTTY_KEY_F20;
    case KEY_F21: return GHOSTTY_KEY_F21;
    case KEY_F22: return GHOSTTY_KEY_F22;
    case KEY_F23: return GHOSTTY_KEY_F23;
    case KEY_F24: return GHOSTTY_KEY_F24;
    default: return GHOSTTY_KEY_UNIDENTIFIED;
    }
  }

  GhosttyKey fromKeysym(std::uint32_t keysym) {
    switch (keysym) {
    case XKB_KEY_Return: return GHOSTTY_KEY_ENTER;
    case XKB_KEY_KP_Enter: return GHOSTTY_KEY_NUMPAD_ENTER;
    case XKB_KEY_Tab: return GHOSTTY_KEY_TAB;
    case XKB_KEY_ISO_Left_Tab: return GHOSTTY_KEY_TAB;
    case XKB_KEY_BackSpace: return GHOSTTY_KEY_BACKSPACE;
    case XKB_KEY_Escape: return GHOSTTY_KEY_ESCAPE;
    case XKB_KEY_Delete: return GHOSTTY_KEY_DELETE;
    case XKB_KEY_Insert: return GHOSTTY_KEY_INSERT;
    case XKB_KEY_Home: return GHOSTTY_KEY_HOME;
    case XKB_KEY_End: return GHOSTTY_KEY_END;
    case XKB_KEY_Page_Up: return GHOSTTY_KEY_PAGE_UP;
    case XKB_KEY_Page_Down: return GHOSTTY_KEY_PAGE_DOWN;
    case XKB_KEY_Up: return GHOSTTY_KEY_ARROW_UP;
    case XKB_KEY_Down: return GHOSTTY_KEY_ARROW_DOWN;
    case XKB_KEY_Left: return GHOSTTY_KEY_ARROW_LEFT;
    case XKB_KEY_Right: return GHOSTTY_KEY_ARROW_RIGHT;
    case XKB_KEY_F1: return GHOSTTY_KEY_F1;
    case XKB_KEY_F2: return GHOSTTY_KEY_F2;
    case XKB_KEY_F3: return GHOSTTY_KEY_F3;
    case XKB_KEY_F4: return GHOSTTY_KEY_F4;
    case XKB_KEY_F5: return GHOSTTY_KEY_F5;
    case XKB_KEY_F6: return GHOSTTY_KEY_F6;
    case XKB_KEY_F7: return GHOSTTY_KEY_F7;
    case XKB_KEY_F8: return GHOSTTY_KEY_F8;
    case XKB_KEY_F9: return GHOSTTY_KEY_F9;
    case XKB_KEY_F10: return GHOSTTY_KEY_F10;
    case XKB_KEY_F11: return GHOSTTY_KEY_F11;
    case XKB_KEY_F12: return GHOSTTY_KEY_F12;
    case XKB_KEY_F13: return GHOSTTY_KEY_F13;
    case XKB_KEY_F14: return GHOSTTY_KEY_F14;
    case XKB_KEY_F15: return GHOSTTY_KEY_F15;
    case XKB_KEY_F16: return GHOSTTY_KEY_F16;
    case XKB_KEY_F17: return GHOSTTY_KEY_F17;
    case XKB_KEY_F18: return GHOSTTY_KEY_F18;
    case XKB_KEY_F19: return GHOSTTY_KEY_F19;
    case XKB_KEY_F20: return GHOSTTY_KEY_F20;
    case XKB_KEY_F21: return GHOSTTY_KEY_F21;
    case XKB_KEY_F22: return GHOSTTY_KEY_F22;
    case XKB_KEY_F23: return GHOSTTY_KEY_F23;
    case XKB_KEY_F24: return GHOSTTY_KEY_F24;
    case XKB_KEY_KP_0: return GHOSTTY_KEY_NUMPAD_0;
    case XKB_KEY_KP_1: return GHOSTTY_KEY_NUMPAD_1;
    case XKB_KEY_KP_2: return GHOSTTY_KEY_NUMPAD_2;
    case XKB_KEY_KP_3: return GHOSTTY_KEY_NUMPAD_3;
    case XKB_KEY_KP_4: return GHOSTTY_KEY_NUMPAD_4;
    case XKB_KEY_KP_5: return GHOSTTY_KEY_NUMPAD_5;
    case XKB_KEY_KP_6: return GHOSTTY_KEY_NUMPAD_6;
    case XKB_KEY_KP_7: return GHOSTTY_KEY_NUMPAD_7;
    case XKB_KEY_KP_8: return GHOSTTY_KEY_NUMPAD_8;
    case XKB_KEY_KP_9: return GHOSTTY_KEY_NUMPAD_9;
    case XKB_KEY_KP_Add: return GHOSTTY_KEY_NUMPAD_ADD;
    case XKB_KEY_KP_Subtract: return GHOSTTY_KEY_NUMPAD_SUBTRACT;
    case XKB_KEY_KP_Multiply: return GHOSTTY_KEY_NUMPAD_MULTIPLY;
    case XKB_KEY_KP_Divide: return GHOSTTY_KEY_NUMPAD_DIVIDE;
    case XKB_KEY_KP_Decimal: return GHOSTTY_KEY_NUMPAD_DECIMAL;
    case XKB_KEY_KP_Separator: return GHOSTTY_KEY_NUMPAD_SEPARATOR;
    case XKB_KEY_KP_Equal: return GHOSTTY_KEY_NUMPAD_EQUAL;
    case XKB_KEY_KP_Up: return GHOSTTY_KEY_NUMPAD_UP;
    case XKB_KEY_KP_Down: return GHOSTTY_KEY_NUMPAD_DOWN;
    case XKB_KEY_KP_Left: return GHOSTTY_KEY_NUMPAD_LEFT;
    case XKB_KEY_KP_Right: return GHOSTTY_KEY_NUMPAD_RIGHT;
    case XKB_KEY_KP_Home: return GHOSTTY_KEY_NUMPAD_HOME;
    case XKB_KEY_KP_End: return GHOSTTY_KEY_NUMPAD_END;
    case XKB_KEY_KP_Page_Up: return GHOSTTY_KEY_NUMPAD_PAGE_UP;
    case XKB_KEY_KP_Page_Down: return GHOSTTY_KEY_NUMPAD_PAGE_DOWN;
    case XKB_KEY_KP_Insert: return GHOSTTY_KEY_NUMPAD_INSERT;
    case XKB_KEY_KP_Delete: return GHOSTTY_KEY_NUMPAD_DELETE;
    case XKB_KEY_KP_Begin: return GHOSTTY_KEY_NUMPAD_BEGIN;
    case XKB_KEY_Shift_L: return GHOSTTY_KEY_SHIFT_LEFT;
    case XKB_KEY_Shift_R: return GHOSTTY_KEY_SHIFT_RIGHT;
    case XKB_KEY_Control_L: return GHOSTTY_KEY_CONTROL_LEFT;
    case XKB_KEY_Control_R: return GHOSTTY_KEY_CONTROL_RIGHT;
    case XKB_KEY_Alt_L: return GHOSTTY_KEY_ALT_LEFT;
    case XKB_KEY_Alt_R: return GHOSTTY_KEY_ALT_RIGHT;
    case XKB_KEY_Super_L: return GHOSTTY_KEY_META_LEFT;
    case XKB_KEY_Super_R: return GHOSTTY_KEY_META_RIGHT;
    case XKB_KEY_Caps_Lock: return GHOSTTY_KEY_CAPS_LOCK;
    case XKB_KEY_Num_Lock: return GHOSTTY_KEY_NUM_LOCK;
    case XKB_KEY_Scroll_Lock: return GHOSTTY_KEY_SCROLL_LOCK;
    case XKB_KEY_Pause: return GHOSTTY_KEY_PAUSE;
    case XKB_KEY_Print: return GHOSTTY_KEY_PRINT_SCREEN;
    case XKB_KEY_Menu: return GHOSTTY_KEY_CONTEXT_MENU;
    default: return GHOSTTY_KEY_UNIDENTIFIED;
    }
  }

  GhosttyKey fromCodepoint(std::uint32_t cp) {
    if (cp >= U'a' && cp <= U'z') {
      return static_cast<GhosttyKey>(GHOSTTY_KEY_A + (cp - U'a'));
    }
    if (cp >= U'1' && cp <= U'9') {
      return static_cast<GhosttyKey>(GHOSTTY_KEY_DIGIT_1 + (cp - U'1'));
    }
    switch (cp) {
    case U'0': return GHOSTTY_KEY_DIGIT_0;
    case U'`': return GHOSTTY_KEY_BACKQUOTE;
    case U'-': return GHOSTTY_KEY_MINUS;
    case U'=': return GHOSTTY_KEY_EQUAL;
    case U'[': return GHOSTTY_KEY_BRACKET_LEFT;
    case U']': return GHOSTTY_KEY_BRACKET_RIGHT;
    case U'\\': return GHOSTTY_KEY_BACKSLASH;
    case U';': return GHOSTTY_KEY_SEMICOLON;
    case U'\'': return GHOSTTY_KEY_QUOTE;
    case U',': return GHOSTTY_KEY_COMMA;
    case U'.': return GHOSTTY_KEY_PERIOD;
    case U'/': return GHOSTTY_KEY_SLASH;
    case U' ': return GHOSTTY_KEY_SPACE;
    default: return GHOSTTY_KEY_UNIDENTIFIED;
    }
  }

  } // namespace

  GhosttyKey ghosttyKey(std::uint32_t evdev, std::uint32_t keysym, std::uint32_t unshifted) {
    if (const GhosttyKey key = fromKeysym(keysym); key != GHOSTTY_KEY_UNIDENTIFIED) {
      return key;
    }
    if (const GhosttyKey key = fromCodepoint(unshifted); key != GHOSTTY_KEY_UNIDENTIFIED) {
      return key;
    }
    return fromEvdev(evdev);
  }

} // namespace term
