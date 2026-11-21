#include "ui/controls/reorder_drag.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <print>
#include <random>
#include <vector>

namespace {

  int g_failures = 0;

  void check(bool ok, const char* what, unsigned seed, int step) {
    if (!ok) {
      ++g_failures;
      std::println(stderr, "reorder_drag_test: {} (seed {}, step {})", what, seed, step);
    }
  }

  std::vector<ReorderDrag::Row> layout(const std::vector<float>& heights, float top, float gap) {
    std::vector<ReorderDrag::Row> rows;
    for (const float height : heights) {
      rows.push_back({.top = top, .height = height});
      top += height + gap;
    }
    return rows;
  }

  // Random press / motion / release / settle / cancel sequences on lists of 1-8 rows of mixed height, run at
  // scale 1 and at a fractional scale in lockstep.
  void runSeed(unsigned seed) {
    std::mt19937 rng(seed);
    const auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    const float scale = 1.0F + static_cast<float>(pick(1, 8)) * 0.25F;
    constexpr float kSlop = 8.0F;
    constexpr float kGap = 4.0F;

    std::vector<float> heights;
    for (int i = pick(1, 8); i > 0; --i) {
      heights.push_back(static_cast<float>(pick(20, 60)));
    }
    std::vector<int> order(heights.size());
    std::iota(order.begin(), order.end(), 0);

    ReorderDrag drag(kSlop);
    ReorderDrag scaled(kSlop * scale);
    const auto currentHeights = [&](float s) {
      std::vector<float> out;
      for (const int item : order) {
        out.push_back(heights[static_cast<std::size_t>(item)] * s);
      }
      return out;
    };

    for (int step = 0; step < 200; ++step) {
      const auto rows = layout(currentHeights(1.0F), 10.0F, kGap);
      const auto scaledRows = layout(currentHeights(scale), 10.0F * scale, kGap * scale);
      switch (pick(0, 5)) {
      case 0: {
        const auto row = static_cast<std::size_t>(pick(0, static_cast<int>(rows.size()) - 1));
        const float y = rows[row].top + rows[row].height * 0.5F;
        drag.press(row, y, rows);
        scaled.press(row, y * scale, scaledRows);
        break;
      }
      case 1:
      case 2: {
        const float y = static_cast<float>(pick(-100, 600));
        drag.motion(y);
        scaled.motion(y * scale);
        break;
      }
      case 3:
        drag.release();
        scaled.release();
        break;
      case 4: {
        const ReorderDrag::Phase before = drag.phase();
        std::vector<float> expectedTops;
        const std::vector<int> previous = order;
        std::vector<int> moved = order;
        if (before == ReorderDrag::Phase::Settling) {
          (void)applyReorder(moved, ReorderDrag::Move{.from = *drag.liftedRow(), .insertion = drag.insertion()});
          for (std::size_t i = 0; i < rows.size(); ++i) {
            expectedTops.push_back(rows[i].top + drag.offsetFor(i));
          }
        }
        const auto move = drag.settled();
        const auto scaledMove = scaled.settled();
        check(move.has_value() == scaledMove.has_value(), "scale changes whether a drag commits", seed, step);
        check(!move.has_value() || before == ReorderDrag::Phase::Settling, "commit outside Settling", seed, step);
        check(drag.phase() == ReorderDrag::Phase::Idle || before != ReorderDrag::Phase::Settling,
              "settle does not end the drag", seed, step);
        check(!drag.settled().has_value(), "a drag commits twice", seed, step);
        if (move.has_value()) {
          check(scaledMove->from == move->from && scaledMove->insertion == move->insertion,
                "scale changes the move", seed, step);
          check(applyReorder(order, *move), "a committed move changes nothing", seed, step);
          check(order == moved, "commit differs from the settled insertion", seed, step);
        }
        if (before == ReorderDrag::Phase::Settling) {
          // Each row's settle target is its slot in the committed order.
          const auto after = layout(currentHeights(1.0F), 10.0F, kGap);
          for (std::size_t i = 0; i < previous.size(); ++i) {
            const auto k = static_cast<std::size_t>(std::ranges::find(order, previous[i]) - order.begin());
            check(std::abs(after[k].top - expectedTops[i]) < 0.01F, "a row settles off its new slot", seed, step);
          }
        }
        break;
      }
      default: {
        drag.cancel();
        scaled.cancel();
        check(!drag.settled().has_value(), "cancel commits", seed, step);
        (void)scaled.settled();
        break;
      }
      }
      check(drag.phase() == scaled.phase(), "scale changes the phase", seed, step);
      check(drag.liftedRow().has_value()
                == (drag.phase() == ReorderDrag::Phase::Dragging || drag.phase() == ReorderDrag::Phase::Settling),
            "lifted row outside a drag", seed, step);
      if (drag.phase() == ReorderDrag::Phase::Dragging) {
        check(drag.insertion() == scaled.insertion(), "scale changes the insertion", seed, step);
        check(drag.insertion() <= rows.size(), "insertion out of range", seed, step);
        // The insertion puts the lifted row between the other rows whose midpoints lie above and below its center.
        const std::size_t from = *drag.liftedRow();
        const std::size_t target = drag.insertion() > from ? drag.insertion() - 1 : drag.insertion();
        const float center = rows[from].top + drag.offsetFor(from) + rows[from].height * 0.5F;
        std::size_t other = 0;
        for (std::size_t i = 0; i < rows.size(); ++i) {
          if (i == from) {
            continue;
          }
          const float mid = rows[i].top + rows[i].height * 0.5F;
          check(other < target ? mid < center : mid >= center, "insertion does not follow the pointer", seed, step);
          ++other;
        }
      }
    }
  }

} // namespace

int main() {
  for (unsigned seed = 1; seed <= 2000; ++seed) {
    runSeed(seed);
  }
  return g_failures == 0 ? 0 : 1;
}
