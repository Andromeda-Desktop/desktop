#pragma once

#include "render/core/renderer.h"

#include <cstdint>

// Inputs a recycled slot's layout reads without marking it dirty. A virtual list lays out a slot again only when its
// binding, its own dirty flag or these change, so a scroll that only moves the content skips the slot.
struct SlotLayoutInputs {
  float width = -1.0F;
  float height = -1.0F;
  float renderScale = 0.0F;
  float textScale = 0.0F;
  std::uint64_t textMetricsGeneration = 0;

  static SlotLayoutInputs of(const Renderer& renderer, float width, float height) {
    return SlotLayoutInputs{
        .width = width,
        .height = height,
        .renderScale = renderer.renderScale(),
        .textScale = renderer.textScale(),
        .textMetricsGeneration = renderer.textMetricsGeneration(),
    };
  }
  bool operator==(const SlotLayoutInputs&) const = default;
};
