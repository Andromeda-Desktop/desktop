#pragma once

#include "ui/controls/box.h"
#include "ui/controls/label.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

class Renderer;

// The one count badge (rulebook 9.4, component.badge): dock unread and window counts, the bar bell.
namespace count_badge {

  [[nodiscard]] inline std::string text(std::size_t count) { return count > 99 ? "99+" : std::to_string(count); }

  // Sets the count, then sizes the capsule `height` tall and at least square with the label centred. Returns its width.
  inline float layout(Renderer& renderer, Box& badge, Label& label, std::size_t count, float height, float scale) {
    label.setText(text(count));
    label.measure(renderer);
    const float width = std::max(height, std::round(label.width() + 2.0F * Tokens::componentBadgePaddingX * scale));
    badge.setFrameSize(width, height);
    badge.setRadius(height * 0.5F);
    label.setPosition(std::round((width - label.width()) * 0.5F), std::round((height - label.height()) * 0.5F));
    return width;
  }

} // namespace count_badge
