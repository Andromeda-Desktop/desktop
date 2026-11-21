#include "platform/popup_place.h"

#include <cstdio>
#include <random>

namespace {
  int g_failures = 0;

  void check(bool ok, const char* what, const PopupSurfaceConfig& c, const PopupBox& box) {
    if (!ok) {
      ++g_failures;
      std::fprintf(stderr, "FAIL %s: anchor=%u gravity=%u adjust=%u rect=%d,%d %dx%d size=%ux%u -> %d,%d\n", what,
                   static_cast<unsigned>(c.anchor), static_cast<unsigned>(c.gravity), c.constraintAdjustment, c.anchorX,
                   c.anchorY, c.anchorWidth, c.anchorHeight, c.width, c.height, box.x, box.y);
    }
  }

  bool inside(const PopupBox& box, const PopupBox& bounds) {
    return box.x >= bounds.x && box.y >= bounds.y && box.x + box.width <= bounds.x + bounds.width
        && box.y + box.height <= bounds.y + bounds.height;
  }
} // namespace

// Random anchors, gravities, sizes and offsets on a 1920x1080 output with the parent anywhere on it.
int main() {
  std::mt19937 rng(7);
  const PopupBox bounds{.x = 0, .y = 25, .width = 1920, .height = 1055};
  auto pick = [&rng](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
  for (int i = 0; i < 20000; ++i) {
    PopupSurfaceConfig c;
    c.anchor = static_cast<PopupAnchor>(pick(0, 8));
    c.gravity = static_cast<PopupGravity>(pick(0, 8));
    c.anchorX = pick(-50, 900);
    c.anchorY = pick(-50, 700);
    c.anchorWidth = pick(1, 300);
    c.anchorHeight = pick(1, 60);
    c.width = static_cast<std::uint32_t>(pick(1, 1900));
    c.height = static_cast<std::uint32_t>(pick(1, 1000));
    c.offsetX = pick(-20, 20);
    c.offsetY = pick(-20, 20);
    c.constraintAdjustment = static_cast<std::uint32_t>(pick(0, 15));
    const int parentX = pick(-200, 1800);
    const int parentY = pick(0, 1000);
    const PopupBox box = placePopup(c, parentX, parentY, bounds);

    check(box.width == static_cast<int>(c.width) && box.height == static_cast<int>(c.height), "keeps size", c, box);
    const std::uint32_t slide = PopupConstraint::SlideX | PopupConstraint::SlideY;
    if ((c.constraintAdjustment & slide) == slide) {
      check(inside(box, bounds), "slide keeps the popup on the output", c, box);
    }
  }
  // A menu below its anchor (anchor and gravity Bottom) with only FlipY allowed.
  for (int i = 0; i < 20000; ++i) {
    PopupSurfaceConfig c;
    c.anchor = PopupAnchor::Bottom;
    c.gravity = PopupGravity::Bottom;
    c.constraintAdjustment = PopupConstraint::FlipY;
    c.anchorY = pick(0, 1000);
    c.anchorHeight = pick(1, 60);
    c.height = static_cast<std::uint32_t>(pick(1, 900));
    c.width = 100;
    const PopupBox box = placePopup(c, 0, 0, bounds);
    const int anchorTop = c.anchorY;
    const int anchorBottom = anchorTop + c.anchorHeight;
    const bool below = anchorBottom + box.height <= bounds.y + bounds.height;
    const bool above = anchorTop - box.height >= bounds.y;
    if (!below && above) {
      check(box.y + box.height == anchorTop, "flip puts a menu that does not fit below above its anchor", c, box);
    } else {
      check(box.y == anchorBottom, "a menu stays below its anchor unless flipping makes it fit", c, box);
    }
  }
  return g_failures == 0 ? 0 : 1;
}
