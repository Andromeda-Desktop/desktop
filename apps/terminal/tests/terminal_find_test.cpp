#include "terminal_find.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <random>
#include <regex>
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

// Plain needles go to libghostty (overlapping, ASCII case-insensitive); the own matcher takes the rest
// (non-overlapping, like PCRE). "(" is not a valid pattern.
const std::vector<std::string> kNeedles = {"ab", "cab", "ba", "zz", "a", "A", "zA", "a+", "[bc]a", "z.", "("};
const std::vector<FindOptions> kOptions = {{}, {.matchCase = true}, {.regex = true}, {.regex = true, .matchCase = true}};

struct Match {
  std::size_t start;
  std::size_t length;
};

std::vector<Match> matches(const std::string& line, const std::string& needle, FindOptions options) {
  std::vector<Match> out;
  if (needle.empty()) {
    return out;
  }
  if (options.regex) {
    const auto flags = options.matchCase ? std::regex::ECMAScript : std::regex::ECMAScript | std::regex::icase;
    const std::regex re(needle, flags);
    for (auto it = std::sregex_iterator(line.begin(), line.end(), re); it != std::sregex_iterator(); ++it) {
      if (it->length() > 0) {
        out.push_back({static_cast<std::size_t>(it->position()), static_cast<std::size_t>(it->length())});
      }
    }
    return out;
  }
  const auto fold = [&](std::string s) {
    if (!options.matchCase) {
      std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
    }
    return s;
  };
  const std::string hay = fold(line);
  const std::string pin = fold(needle);
  const std::size_t advance = options == FindOptions{} ? 1 : pin.size();
  for (auto at = hay.find(pin); at != std::string::npos; at = hay.find(pin, at + advance)) {
    out.push_back({at, pin.size()});
  }
  return out;
}

bool valid(const std::string& needle, FindOptions options) {
  return !options.regex || needle != "(";
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

// A logical line of output and its first screen row; it wraps every kCols cells.
struct Line {
  std::string text;
  std::uint64_t row;
};

void checkInvariants(const TerminalFind& find, GhosttyTerminal t, const std::vector<Line>& lines,
                     const std::string& needle, FindOptions options) {
  GhosttyTerminalScrollbar scroll{};
  ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &scroll);
  const bool ok = valid(needle, options);
  CHECK(find.error() == (ok ? FindError::None : FindError::InvalidPattern));
  // Screen hits of every match: viewport ones must be hits, and every hit must be one of them.
  std::size_t expected = 0;
  std::vector<std::uint64_t> matchRows;
  std::vector<FindHit> inside;   // start and end in the viewport
  std::vector<FindHit> starting; // start in the viewport
  for (const Line& line : lines) {
    for (const Match& m : ok ? matches(line.text, needle, options) : std::vector<Match>{}) {
      ++expected;
      const std::uint64_t r0 = line.row + m.start / kCols;
      const std::uint64_t r1 = line.row + (m.start + m.length - 1) / kCols;
      matchRows.push_back(r0);
      if (r0 < scroll.offset || r0 >= scroll.offset + scroll.len) {
        continue;
      }
      const FindHit h{.start = {.x = static_cast<std::uint16_t>(m.start % kCols), .y = static_cast<std::uint32_t>(r0 - scroll.offset)},
                      .end = {.x = static_cast<std::uint16_t>((m.start + m.length - 1) % kCols),
                              .y = static_cast<std::uint32_t>(r1 - scroll.offset)}};
      starting.push_back(h);
      if (r1 < scroll.offset + scroll.len) {
        inside.push_back(h);
      }
    }
  }
  CHECK(find.total() == expected);
  CHECK(!find.index() || *find.index() < find.total());
  CHECK(find.index().has_value() == (find.total() > 0));
  for (const FindHit& h : inside) {
    CHECK(std::ranges::find(find.hits(), h) != find.hits().end());
  }
  for (const FindHit& h : find.hits()) {
    const bool known = std::ranges::any_of(starting, [&](const FindHit& m) {
      return m.start.x == h.start.x && m.start.y == h.start.y && m.end.y == h.end.y && (m.end.x == h.end.x || h.end.y >= kRows);
    });
    CHECK(known);
  }
  for (std::uint32_t row = 0; row < kRows; ++row) {
    for (std::uint16_t col = 0; col < kCols; ++col) {
      const auto covers = [&](const FindHit& h) {
        return (row > h.start.y || (row == h.start.y && col >= h.start.x)) && (row < h.end.y || (row == h.end.y && col <= h.end.x));
      };
      bool current = false;
      const bool hit = find.hitAt(row, col, current);
      CHECK(hit == std::ranges::any_of(find.hits(), covers));
      CHECK(current == (find.current() && covers(*find.current())));
    }
  }
  if (find.current()) {
    CHECK(std::ranges::find(find.hits(), *find.current()) != find.hits().end());
  }
  CHECK(find.marks().empty() == (find.total() == 0));
  CHECK(find.marks().size() <= find.total());
  CHECK(std::ranges::all_of(find.marks(), [](float f) { return f >= 0.0F && f < 1.0F; }));
  for (const float f : find.marks()) {
    const auto row = static_cast<std::uint64_t>(f * static_cast<float>(scroll.total) + 0.5F);
    CHECK(std::ranges::find(matchRows, row) != matchRows.end());
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
  std::vector<Line> lines{{"", 0}}; // the cursor line last
  std::optional<TerminalFind> find;
  std::string needle;
  FindOptions options;
  const auto pick = [&](std::size_t n) { return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng); };
  for (step = 0; step < 300; ++step) {
    switch (pick(6)) {
      case 0:
      case 1: { // one line of output; one in four wraps
        std::string line;
        for (std::size_t i = pick(pick(4) == 0 ? 3 * kCols : kCols - 1); i > 0; --i) {
          line += "abczA "[pick(6)];
        }
        lines.back().text = line;
        const std::uint64_t rows = line.empty() ? 1 : (line.size() + kCols - 1) / kCols;
        lines.push_back({"", lines.back().row + rows});
        line += "\r\n";
        ghostty_terminal_vt_write(t, reinterpret_cast<const std::uint8_t*>(line.data()), line.size());
        break;
      }
      case 2:
        if (!find) {
          find.emplace();
        }
        needle = pick(5) == 0 ? std::string() : kNeedles[pick(kNeedles.size())];
        options = kOptions[pick(kOptions.size())];
        if (options == FindOptions{} && needle.find_first_of("+[.(") != std::string::npos) {
          options.regex = true; // libghostty would take the pattern literally
        }
        find->setQuery(t, needle, options);
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
      find->contentChanged(); // the view's rescan frame
      settle(*find, t);
      checkInvariants(*find, t, lines, needle, options);
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
