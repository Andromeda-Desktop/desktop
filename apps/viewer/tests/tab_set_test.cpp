#include "tab_set.h"

#include <algorithm>
#include <print>
#include <random>
#include <set>
#include <string>

namespace {
  int g_failures = 0;

  void expect(bool ok, const std::string& what, unsigned step) {
    if (!ok) {
      std::println(stderr, "tab_set_test: step {}: {}", step, what);
      ++g_failures;
    }
  }

  void checkInvariants(const viewer::TabSet& set, unsigned step) {
    const auto& tabs = set.tabs();
    expect(std::set(tabs.begin(), tabs.end()).size() == tabs.size(), "ids are distinct", step);
    expect(set.focused().has_value() == !tabs.empty(), "focus is set exactly when a tab is open", step);
    expect(!set.focused() || set.contains(*set.focused()), "the focused tab is open", step);
    std::set<std::uint64_t> members;
    for (const auto& split : set.splits()) {
      expect(split.size() >= 2 && split.size() <= viewer::kMaxPanes, "a split holds 2..4 panes", step);
      for (const auto id : split) {
        expect(set.contains(id), "a split member is open", step);
        expect(members.insert(id).second, "a tab is in at most one split", step);
      }
    }
    const auto shown = set.visible();
    expect(tabs.empty() == shown.empty(), "an open window shows a pane", step);
    expect(!set.focused() || std::ranges::find(shown, *set.focused()) != shown.end(), "the focused tab is visible", step);
  }
} // namespace

int main() {
  using namespace viewer::tab_event;

  // Random event sequences over a small id space reach every mode: blank, one tab, split, full split, focus outside.
  std::mt19937 rng(7);
  for (unsigned run = 0; run < 200; ++run) {
    viewer::TabSet set;
    for (unsigned step = 0; step < 200; ++step) {
      const auto id = [&] { return static_cast<std::uint64_t>(rng() % 7); };
      const auto before = set;
      const auto beforeFocus = set.focused();
      bool applied = false;
      switch (rng() % 5) {
      case 0: {
        const auto e = Open{id()};
        applied = set.apply(e);
        expect(!applied || set.focused() == e.id, "Open focuses the new tab", step);
        break;
      }
      case 1: {
        const auto e = Close{id()};
        const auto* split = before.splitOf(e.id);
        const bool closingFocused = beforeFocus == e.id;
        applied = set.apply(e);
        expect(!applied || !set.contains(e.id), "Close removes the tab", step);
        if (applied && closingFocused && split != nullptr) {
          expect(std::ranges::find(*split, *set.focused()) != split->end(), "focus stays in the closed pane's split", step);
        }
        break;
      }
      case 2: {
        const auto e = Focus{id()};
        applied = set.apply(e);
        expect(!applied || set.focused() == e.id, "Focus focuses", step);
        break;
      }
      case 3: {
        const auto e = Join{id(), id(), rng() % 2 == 0};
        applied = set.apply(e);
        if (applied) {
          const auto* split = set.splitOf(e.joining);
          expect(split != nullptr && split == set.splitOf(e.target), "Join puts both tabs in one split", step);
          expect(set.focused() == e.joining, "Join focuses the joining tab", step);
          if (split != nullptr) {
            const auto t = std::ranges::find(*split, e.target) - split->begin();
            const auto j = std::ranges::find(*split, e.joining) - split->begin();
            expect(j == (e.before ? t - 1 : t + 1), "Join places the tab beside its target", step);
          }
        }
        break;
      }
      default: {
        const auto e = Leave{id()};
        applied = set.apply(e);
        expect(!applied || set.splitOf(e.id) == nullptr, "Leave takes the tab out of its split", step);
        expect(!applied || set.focused() == beforeFocus, "Leave keeps focus", step);
        break;
      }
      }
      if (!applied) {
        expect(set.tabs() == before.tabs() && set.focused() == before.focused() && set.splits() == before.splits(),
               "a rejected event changes nothing", step);
      }
      checkInvariants(set, step);
    }
  }

  return g_failures == 0 ? 0 : 1;
}
