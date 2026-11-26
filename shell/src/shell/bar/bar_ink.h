#pragma once

#include "render/scene/node.h"

#include <cstdint>

// Ink of a bare bar, picked from the backdrop luminance under it. Light: white ink over the dark scrim.
enum class BarInk : std::uint8_t { Light, Dark };

// Flips only past `flip` by more than `deadband`, so a backdrop near the threshold does not flicker.
[[nodiscard]] constexpr BarInk nextBarInk(BarInk ink, float luminance, float flip, float deadband) noexcept {
  if (ink == BarInk::Light && luminance > flip + deadband) {
    return BarInk::Dark;
  }
  if (ink == BarInk::Dark && luminance < flip - deadband) {
    return BarInk::Light;
  }
  return ink;
}

// The dark palette carries white ink.
[[nodiscard]] constexpr PaletteTone barInkTone(BarInk ink) noexcept {
  return ink == BarInk::Light ? PaletteTone::Dark : PaletteTone::Light;
}
