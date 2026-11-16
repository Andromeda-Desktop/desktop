#include "render/scene/terminal_bidi.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <random>
#include <vector>

namespace {

int failures = 0;
unsigned seed = 0;
int step = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s (seed %u step %d)\n", __FILE__, __LINE__, #c, seed, step); ++failures; } } while (0)

bool latin(char32_t c) { return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z'); }
bool hebrew(char32_t c) { return c >= 0x5D0 && c <= 0x5EA; }
bool arabic(char32_t c) { return c >= 0x621 && c <= 0x64A; }

std::vector<TerminalCell> cellsOf(std::u32string_view text) {
  std::vector<TerminalCell> row;
  for (const char32_t c : text) {
    row.push_back({.codepoint = c, .fg = static_cast<std::uint32_t>(row.size())});
  }
  return row;
}

void checkInvariants(const std::vector<TerminalCell>& row) {
  bool rtl = false;
  for (const auto& cell : row) {
    rtl = rtl || hebrew(cell.codepoint) || arabic(cell.codepoint);
  }
  const auto bidi = terminalBidi(row);
  CHECK(bidi.has_value() == rtl);
  if (!bidi) {
    return;
  }
  const std::size_t n = row.size();
  CHECK(bidi->cells.size() == n && bidi->logical.size() == n);
  if (bidi->cells.size() != n || bidi->logical.size() != n) {
    return;
  }
  std::vector<int> visualOf(n, -1);
  for (std::size_t v = 0; v < n; ++v) {
    const std::uint16_t l = bidi->logical[v];
    CHECK(l < n && visualOf[l] == -1);
    if (l >= n || visualOf[l] != -1) {
      return;
    }
    visualOf[l] = static_cast<int>(v);
    TerminalCell drawn = bidi->cells[v];
    const TerminalCell& source = row[l];
    // Only an Arabic letter (joined form) or a mirrored bracket changes its codepoint.
    if (drawn.codepoint != source.codepoint) {
      CHECK(arabic(source.codepoint) || source.codepoint == U'(' || source.codepoint == U')' || source.codepoint == U'['
            || source.codepoint == U']');
      drawn.codepoint = source.codepoint;
    }
    CHECK(drawn == source);
  }
  for (std::size_t l = 0; l + 1 < n; ++l) {
    if ((row[l].flags & TerminalCell::Wide) != 0 && (l == 0 || (row[l - 1].flags & TerminalCell::Wide) == 0)) {
      CHECK(visualOf[l + 1] == visualOf[l] + 1);
    }
    if (latin(row[l].codepoint) && latin(row[l + 1].codepoint)) {
      CHECK(visualOf[l + 1] == visualOf[l] + 1);
    }
    if (hebrew(row[l].codepoint) && hebrew(row[l + 1].codepoint)) {
      CHECK(visualOf[l + 1] == visualOf[l] - 1);
    }
  }
  if (latin(row[0].codepoint)) {
    CHECK(visualOf[0] == 0);
  }
}

} // namespace

int main() {
  {
    const auto bidi = terminalBidi(cellsOf(U"ab אבג cd"));
    CHECK(bidi.has_value());
    std::u32string shown;
    for (const auto& cell : bidi.value_or(TerminalBidiRow{}).cells) {
      shown += cell.codepoint;
    }
    CHECK(shown == U"ab גבא cd");
  }
  {
    // سلام: initial seen, medial lam, final alef... drawn as presentation forms, right to left.
    const auto bidi = terminalBidi(cellsOf(U"سلام"));
    CHECK(bidi.has_value());
    for (const auto& cell : bidi.value_or(TerminalBidiRow{}).cells) {
      CHECK(cell.codepoint >= 0xFE70 && cell.codepoint <= 0xFEFF);
    }
    CHECK(bidi && bidi->logical == (std::vector<std::uint16_t>{3, 2, 1, 0}));
  }
  const char32_t pool[] = {U'a', U'b', U'Z', U' ', U'1', U'2', U'(', U')', U'[', U'-', U'.', 0,
                           0x5D0, 0x5D3, 0x5E9, 0x628, 0x644, 0x627, 0x645};
  for (seed = 1; seed <= 3000; ++seed) {
    std::mt19937 rng(seed);
    std::vector<TerminalCell> row;
    const int width = 1 + static_cast<int>(rng() % 40);
    while (static_cast<int>(row.size()) < width) {
      step = static_cast<int>(row.size());
      if (rng() % 10 == 0 && static_cast<int>(row.size()) + 1 < width) {
        row.push_back({.codepoint = 0x4E2D, .fg = static_cast<std::uint32_t>(row.size()), .flags = TerminalCell::Wide});
        row.push_back({.codepoint = 0, .fg = static_cast<std::uint32_t>(row.size())});
        continue;
      }
      row.push_back({.codepoint = pool[rng() % std::size(pool)], .fg = static_cast<std::uint32_t>(row.size()),
                     .bg = static_cast<std::uint32_t>(rng() % 3), .flags = static_cast<std::uint16_t>(rng() % 2 ? TerminalCell::Bold : 0)});
    }
    checkInvariants(row);
  }
  std::printf("%s (%d failures)\n", failures == 0 ? "ok" : "FAILED", failures);
  return failures == 0 ? 0 : 1;
}
