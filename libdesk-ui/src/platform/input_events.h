#pragma once

#include <cstdint>

class Surface;

struct PointerEvent {
  enum class Type : std::uint8_t { Enter, Leave, Motion, Button, Axis };
  Type type;
  std::uint32_t serial = 0;
  Surface* surface = nullptr; // null for a surface no Surface owns
  void* nativeSurface = nullptr; // the window system's handle (Wayland: wl_surface*), set for every surface
  double sx = 0.0;
  double sy = 0.0;
  std::uint32_t time = 0;
  std::uint32_t button = 0;
  bool pressed = false;
  std::uint32_t axis = 0;
  std::uint32_t axisSource = 0;
  double axisValue = 0.0;
  std::int32_t axisDiscrete = 0;
  std::int32_t axisValue120 = 0;
  float axisLines = 0.0F;
  std::uint32_t axisGestureSerial = 0;
  // The physical motion is opposite to the axis value (natural scrolling).
  bool axisInverted = false;
  bool touch = false;
};

// A touchpad pinch over the pointer's surface. `scale` is relative to the pinch start.
struct PinchEvent {
  enum class Phase : std::uint8_t { Begin, Update, End };
  Phase phase = Phase::Begin;
  Surface* surface = nullptr; // null for a surface no Surface owns
  void* nativeSurface = nullptr; // the window system's handle (Wayland: wl_surface*), set for every surface
  double sx = 0.0;
  double sy = 0.0;
  double scale = 1.0;
  double dx = 0.0;
  double dy = 0.0;
  bool cancelled = false;
};

struct KeyboardEvent {
  std::uint32_t sym = 0;       // XKB keysym
  std::uint32_t utf32 = 0;     // Unicode codepoint (0 for non-printable keys)
  std::uint32_t key = 0;       // raw Linux keycode
  std::uint32_t unshifted = 0; // codepoint of the key with no modifier in the active layout (0 for no text)
  std::uint32_t modifiers = 0; // KeyMod bitmask
  bool pressed = false;
  bool preedit = false; // dead key preview (composing in progress)
  bool repeat = false;  // synthesized by client-side key repeat, not a fresh press
};

enum class InputSource : std::uint8_t {
  None,
  Pointer,
  Keyboard,
  Touch,
};

struct LockKeysState {
  bool capsLock = false;
  bool numLock = false;
  bool scrollLock = false;

  bool operator==(const LockKeysState&) const = default;
};
