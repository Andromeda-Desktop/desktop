#include "terminal_find.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace term;

namespace {

int failures = 0;
unsigned seed = 0;
int step = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s (seed %u step %d)\n", __FILE__, __LINE__, #c, seed, step); ++failures; } } while (0)

constexpr std::uint16_t kCols = 30;
constexpr std::uint16_t kRows = 6;

// Matches may overlap ("zz" in "zzz" is two), as in libghostty.
const std::vector<std::string> kNeedles = {"ab", "cab", "ba", "zz", "a"};

std::size_t count(const std::string& line, const std::string& needle) {
  std::size_t n = 0;
  for (auto at = line.find(needle); at != std::string::npos; at = line.find(needle, at + 1)) {
    ++n;
  }
  return n;
}

// Like TerminalView::refresh: read under the lock, tick off it, repeat while tick asks for another frame.
// What the find bar shows: updated only when read() reports a change, as TerminalView does.
using Label = std::optional<std::pair<std::size_t, std::optional<std::size_t>>>;
Label label;

void settle(TerminalFind& find, GhosttyTerminal t) {
  for (int frame = 0; frame < 1000; ++frame) {
    if (find.read(t)) {
      label = {{find.total(), find.index()}};
    }
    if (!find.tick(std::chrono::milliseconds(2))) {
      return;
    }
  }
  const bool settled = false;
  CHECK(settled);
}

void checkInvariants(const TerminalFind& find, GhosttyTerminal t, const std::vector<std::string>& lines,
                     const std::string& needle) {
  std::size_t expected = 0;
  for (const auto& line : lines) {
    expected += needle.empty() ? 0 : count(line, needle);
  }
  CHECK(find.total() == expected);
  CHECK(!find.index() || *find.index() < find.total());
  CHECK(find.index().has_value() == (find.total() > 0));
  CHECK(find.hits().size() <= find.total());
  for (const FindHit& h : find.hits()) {
    CHECK(h.start.y < kRows && h.end.y < kRows && h.start.y == h.end.y);
    CHECK(static_cast<std::size_t>(h.end.x - h.start.x + 1) == needle.size());
  }
  if (find.current()) {
    CHECK(std::ranges::find(find.hits(), *find.current()) != find.hits().end());
  }
  // Viewport rows hold lines [offset, offset + len): the hits are exactly the needles there.
  GhosttyTerminalScrollbar scroll{};
  ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &scroll);
  std::size_t visible = 0;
  for (std::uint64_t row = scroll.offset; row < scroll.offset + scroll.len && row < lines.size(); ++row) {
    visible += needle.empty() ? 0 : count(lines[row], needle);
  }
  CHECK(find.hits().size() == visible);
  CHECK(find.marks().empty() == (find.total() == 0));
  CHECK(find.marks().size() <= find.total());
  CHECK(std::ranges::all_of(find.marks(), [](float f) { return f >= 0.0F && f < 1.0F; }));
  for (const float f : find.marks()) {
    const auto row = static_cast<std::size_t>(f * static_cast<float>(scroll.total) + 0.5F);
    CHECK(row < lines.size() && count(lines[row], needle) > 0);
  }
  CHECK(label == Label({find.total(), find.index()}));
  CHECK(find.currentMark().has_value() == find.index().has_value());
}

void run() {
  std::mt19937 rng(seed);
  GhosttyTerminal t = nullptr;
  ghostty_terminal_new(nullptr, &t, kCols, kRows);
  std::size_t scrollback = 1 << 20;
  ghostty_terminal_set(t, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, &scrollback);
  std::vector<std::string> lines{""}; // screen rows, the cursor row last
  std::optional<TerminalFind> find;
  std::string needle;
  const auto pick = [&](std::size_t n) { return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng); };
  for (step = 0; step < 300; ++step) {
    switch (pick(6)) {
      case 0:
      case 1: { // one line of output; never wraps
        std::string line;
        for (std::size_t i = pick(kCols - 1); i > 0; --i) {
          line += "abcz "[pick(5)];
        }
        lines.back() = line;
        lines.emplace_back();
        line += "\r\n";
        ghostty_terminal_vt_write(t, reinterpret_cast<const std::uint8_t*>(line.data()), line.size());
        break;
      }
      case 2:
        if (!find) {
          find.emplace();
        }
        needle = pick(5) == 0 ? std::string() : kNeedles[pick(kNeedles.size())];
        find->setQuery(t, needle);
        label.reset();
        break;
      case 3:
      case 4:
        if (find) {
          const auto before = find->index();
          find->step(pick(2) == 0);
          settle(*find, t);
          if (before && find->total() > 1) {
            CHECK(find->index() != before);
          }
        }
        break;
      default:
        find.reset();
        needle.clear();
        break;
    }
    if (find) {
      settle(*find, t);
      checkInvariants(*find, t, lines, needle);
    }
  }
  find.reset();
  ghostty_terminal_free(t);
}

} // namespace

int main() {
  for (seed = 1; seed <= 40; ++seed) {
    run();
  }
  if (failures == 0) {
    std::puts("ok");
  }
  return failures == 0 ? 0 : 1;
}
