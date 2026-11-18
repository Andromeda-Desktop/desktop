#include "check.h"
#include "config/config.h"
#include "layout/dwindle.h"
#include "layout/master.h"
#include "layout/scrolling.h"

// clang-format off
// See keybind_parse.cpp: <cmath> must precede the wayland chain.
#include <cmath>
#include <algorithm>
#include <map>
#include <memory>
#include <random>
#include <vector>
extern "C" {
#include <wlr/util/box.h>
}
// clang-format on

using andromeda::Layout;
using andromeda::LayoutMode;
using andromeda::ResolvedLayoutConfig;
using andromeda::TileSlot;
using andromeda::View;

// Model test: random open, close, minimize and restore sequences per layout. Invariants after every step:
// a minimized tile is never in the layout, every shown tile is in it exactly once, and restoring the newest
// minimized tile with nothing else changed since gives every tile the box it had before that minimize.

namespace {

  View* stub(int id) { return reinterpret_cast<View*>(static_cast<uintptr_t>(0x5000 + (id * 0x10))); }

  constexpr wlr_box kUsable{0, 0, 1280, 720};
  constexpr int kViews = 7;

  ResolvedLayoutConfig configFor(LayoutMode mode, andromeda::MasterPosition position) {
    ResolvedLayoutConfig config;
    config.mode = mode;
    config.gap = 8;
    config.totalGap = 12;
    config.edgePad = 10;
    config.scrolling.defaultExtentFraction = 0.5;
    config.master.position = position;
    config.master.defaultWidthFraction = 0.55;
    return config;
  }

  std::unique_ptr<Layout> make(LayoutMode mode) {
    switch (mode) {
    case LayoutMode::Dwindle:
      return std::make_unique<andromeda::DwindleLayout>();
    case LayoutMode::Master:
      return std::make_unique<andromeda::MasterStackLayout>();
    default:
      return std::make_unique<andromeda::ScrollingLayout>();
    }
  }

  using Boxes = std::map<const View*, wlr_box>;

  bool same(const Boxes& a, const Boxes& b) {
    return a.size() == b.size() && std::ranges::all_of(a, [&b](const auto& entry) {
             const auto other = b.find(entry.first);
             const wlr_box& x = entry.second;
             return other != b.end()
                 && x.x == other->second.x
                 && x.y == other->second.y
                 && x.width == other->second.width
                 && x.height == other->second.height;
           });
  }

  enum class State : uint8_t { Closed, Shown, Minimized };

  struct Hidden {
    View* view = nullptr;
    TileSlot slot;
    Boxes before;
    bool exact = true; // nothing but nested minimize/restore pairs happened since
  };

  int runModel(LayoutMode mode, andromeda::MasterPosition position, unsigned seed) {
    ResolvedLayoutConfig config = configFor(mode, position);
    std::unique_ptr<Layout> layout = make(mode);
    layout->setConfig(&config);
    std::mt19937 rng(seed);
    std::vector<State> state(kViews, State::Closed);
    std::vector<Hidden> hidden;
    int failures = 0;

    const auto boxes = [&] {
      layout->arrange(kUsable);
      Boxes out;
      for (int i = 0; i < kViews; ++i) {
        if (state[static_cast<size_t>(i)] == State::Shown) {
          out[stub(i)] = layout->targetBox(stub(i));
        }
      }
      return out;
    };
    const auto spoilAll = [&] {
      for (Hidden& entry : hidden) {
        entry.exact = false;
      }
    };

    for (int step = 0; step < 1500; ++step) {
      const int id = static_cast<int>(rng() % kViews);
      State& s = state[static_cast<size_t>(id)];
      const auto entry = std::ranges::find(hidden, stub(id), &Hidden::view);
      switch (rng() % 4) {
      case 0: // open
        if (s == State::Closed) {
          andromeda::ColumnOps* ops = layout->columnOps();
          const auto columns = static_cast<int>(layout->columns().size());
          if (ops != nullptr && columns > 0 && rng() % 2 == 0) {
            ops->insertViewIntoColumn(stub(id), static_cast<int>(rng() % columns), static_cast<int>(rng() % 3));
          } else {
            layout->insertView(stub(id), static_cast<int>(rng() % 4));
          }
          s = State::Shown;
          spoilAll();
        }
        break;
      case 1: // close
        if (s == State::Shown) {
          layout->removeView(stub(id));
          spoilAll();
        } else if (s == State::Minimized) {
          // Older minimized tiles left a layout that still held this one.
          hidden.erase(entry);
          spoilAll();
        }
        s = State::Closed;
        break;
      case 2: // minimize
        if (s == State::Shown) {
          const Boxes before = boxes();
          const std::optional<TileSlot> slot = layout->slotOf(stub(id));
          CHECK(slot.has_value());
          layout->removeView(stub(id));
          s = State::Minimized;
          hidden.push_back({.view = stub(id), .slot = *slot, .before = before});
        }
        break;
      default: // restore
        if (s == State::Minimized) {
          const bool newest = entry + 1 == hidden.end();
          const Hidden restored = *entry;
          hidden.erase(entry);
          CHECK(layout->insertAtSlot(stub(id), restored.slot));
          s = State::Shown;
          if (newest && restored.exact) {
            const bool exact = same(boxes(), restored.before);
            CHECK(exact);
            failures += exact ? 0 : 1;
          } else {
            spoilAll();
          }
        }
        break;
      }

      size_t inLayout = 0;
      for (const auto& column : layout->columns()) {
        inLayout += column.views.size();
      }
      size_t shown = 0;
      for (int i = 0; i < kViews; ++i) {
        const State v = state[static_cast<size_t>(i)];
        const bool present = layout->columnOf(stub(i)) >= 0;
        CHECK_EQ(present, v == State::Shown);
        failures += present == (v == State::Shown) ? 0 : 1;
        shown += v == State::Shown ? 1 : 0;
      }
      CHECK_EQ(inLayout, shown);
      if (failures > 0) {
        return failures;
      }
    }
    return failures;
  }

  void runSeeds(LayoutMode mode, andromeda::MasterPosition position = andromeda::MasterPosition::Left) {
    for (unsigned seed = 1; seed <= 20; ++seed) {
      CHECK_EQ(runModel(mode, position, seed), 0);
    }
  }

} // namespace

ANDROMEDA_TEST(scrollingMinimizedTilesLeaveAndReturnToTheirPlace) { runSeeds(LayoutMode::Scrolling); }

ANDROMEDA_TEST(dwindleMinimizedTilesLeaveAndReturnToTheirPlace) { runSeeds(LayoutMode::Dwindle); }

ANDROMEDA_TEST(masterMinimizedTilesLeaveAndReturnToTheirPlace) { runSeeds(LayoutMode::Master); }

ANDROMEDA_TEST(centeredMasterMinimizedTilesLeaveAndReturnToTheirPlace) {
  runSeeds(LayoutMode::Master, andromeda::MasterPosition::Center);
}

ANDROMEDA_TEST(aSlotFromAnotherLayoutIsRefused) {
  ResolvedLayoutConfig config = configFor(LayoutMode::Scrolling, andromeda::MasterPosition::Left);
  andromeda::ScrollingLayout scrolling;
  scrolling.setConfig(&config);
  scrolling.insertView(stub(0), 0);
  const std::optional<TileSlot> slot = scrolling.slotOf(stub(0));
  CHECK(slot.has_value());
  andromeda::DwindleLayout dwindle;
  dwindle.setConfig(&config);
  CHECK(!dwindle.insertAtSlot(stub(0), *slot));
  CHECK_EQ(dwindle.columnOf(stub(0)), -1);
}

int main() { return RUN_TESTS(); }
