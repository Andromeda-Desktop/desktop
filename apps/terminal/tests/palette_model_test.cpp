#include "palette_model.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace term::palette;

namespace {

int failures = 0;
unsigned seed = 0;
int step = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s (seed %u step %d)\n", __FILE__, __LINE__, #c, seed, step); ++failures; } } while (0)

std::u32string lowered(const std::string& s) {
  std::u32string out;
  for (std::size_t i = 0; i < s.size();) {
    const auto b = static_cast<unsigned char>(s[i]);
    const std::size_t len = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : 4;
    char32_t c = len == 1 ? b : b & (0x7FU >> len);
    for (std::size_t n = 1; n < len; ++n) {
      c = (c << 6) | (static_cast<unsigned char>(s[i + n]) & 0x3F);
    }
    out.push_back(c >= U'A' && c <= U'Z' ? c + 32 : c);
    i += len;
  }
  return out;
}

// The reference: a plain subsequence test, spaces in the query ignored.
bool isSubsequence(const std::string& query, const std::string& text) {
  const std::u32string t = lowered(text);
  std::size_t ti = 0;
  for (const char32_t c : lowered(query)) {
    if (c == U' ') {
      continue;
    }
    while (ti < t.size() && t[ti] != c) {
      ++ti;
    }
    if (ti++ >= t.size()) {
      return false;
    }
  }
  return true;
}

void checkHits(const std::string& query, const std::string& text, const std::vector<std::uint32_t>& hits) {
  const std::u32string t = lowered(text);
  std::u32string q;
  for (const char32_t c : lowered(query)) {
    if (c != U' ') {
      q.push_back(c);
    }
  }
  CHECK(hits.size() == q.size());
  for (std::size_t i = 0; i < hits.size() && i < q.size(); ++i) {
    CHECK(hits[i] < t.size() && t[hits[i]] == q[i]);
    CHECK(i == 0 || hits[i] > hits[i - 1]);
  }
}

const std::vector<std::string> kWords = {"build", "logs", "/etc", "link", "Copy", "Paste", "Split with…", "Leave split",
                                         "Larger text", "сервер-логи", "🚀 release", "deploy-staging-eu", "a", "Dark",
                                         "AMOLED", "theme", "cargo build --release", "/data/desktop/apps/terminal"};

std::string randomText(std::mt19937& rng) {
  std::string out;
  const int words = 1 + static_cast<int>(rng() % 3);
  for (int i = 0; i < words; ++i) {
    out += (i ? " " : "") + kWords[rng() % kWords.size()];
  }
  return out;
}

// A query from a random item's letters (a hit), or random letters (often a miss).
std::string randomQuery(std::mt19937& rng, const std::vector<Item>& items) {
  std::string q;
  if (rng() % 3 != 0) {
    const std::string& src = items[rng() % items.size()].label;
    for (const char c : src) {
      if ((static_cast<unsigned char>(c) < 0x80) && rng() % 3 == 0) {
        q.push_back(c);
      }
    }
  } else {
    for (int i = 0, n = static_cast<int>(rng() % 4); i < n; ++i) {
      q.push_back(static_cast<char>('a' + rng() % 26));
    }
  }
  if (rng() % 6 == 0) {
    q = (rng() % 2 ? "Theme " : "theme") + q;
  }
  return q;
}

} // namespace

int main() {
  for (seed = 1; seed <= 400; ++seed) {
    std::mt19937 rng(seed);
    std::vector<Item> items(1 + rng() % 40);
    for (std::size_t i = 0; i < items.size(); ++i) {
      items[i].kind = static_cast<Kind>(rng() % 3);
      items[i].id = std::to_string(i);
      items[i].label = randomText(rng);
      items[i].sub = rng() % 2 ? randomText(rng) : "";
      items[i].off = rng() % 5 == 0 ? "No selection" : "";
    }
    for (step = 0; step < 20; ++step) {
      std::string query = randomQuery(rng, items);
      const auto ranked = rank(query, items);
      bool themesOnly = query.size() >= 5 && std::equal(query.begin(), query.begin() + 5, "theme",
                                                        [](char a, char b) { return std::tolower(a) == b; });
      const std::string rest = themesOnly ? query.substr(5) : query;
      // Exactly the items that match, each once.
      std::vector<bool> seen(items.size(), false);
      for (const Match& m : ranked) {
        CHECK(m.item < items.size() && !seen[m.item]);
        seen[m.item] = true;
      }
      for (std::size_t i = 0; i < items.size(); ++i) {
        const bool expected = (!themesOnly || items[i].kind == Kind::Theme) &&
                              (isSubsequence(rest, items[i].label) || (!items[i].sub.empty() && isSubsequence(rest, items[i].sub)));
        CHECK(seen[i] == expected);
      }
      // Marked letters spell the query in order; usable rows before off rows; best first in each group.
      for (std::size_t k = 0; k < ranked.size(); ++k) {
        const Match& m = ranked[k];
        const Item& item = items[m.item];
        if (m.subHits.empty()) {
          checkHits(rest, item.label, m.hits);
        } else {
          CHECK(m.hits.empty());
          CHECK(!isSubsequence(rest, item.label));
          checkHits(rest, item.sub, m.subHits);
        }
        if (k > 0) {
          const Match& prev = ranked[k - 1];
          const bool prevOff = !items[prev.item].off.empty();
          const bool off = !item.off.empty();
          CHECK(!prevOff || off);
          CHECK(prevOff != off || prev.score >= m.score);
        }
      }
    }
  }

  // A word start beats a letter run inside a word: "lo" puts "logs" above "hello".
  step = 0;
  const std::vector<Item> words = {{.label = "hello"}, {.label = "logs"}};
  const auto byStart = rank("lo", words);
  CHECK(byStart.size() == 2 && byStart[0].item == 1);

  // Perf budget (FRAME.md): ~200 rows filtered per key in well under 0.3 ms on a desktop CPU.
  std::mt19937 rng(7);
  std::vector<Item> many(200);
  for (Item& item : many) {
    item.label = randomText(rng);
    item.sub = randomText(rng);
  }
  const auto t0 = std::chrono::steady_clock::now();
  constexpr int kRounds = 200;
  for (int i = 0; i < kRounds; ++i) {
    (void)rank(i % 2 ? "re" : "lo", many);
  }
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / kRounds;
  std::printf("rank 200 rows: %.1f us per key\n", us);

  if (failures == 0) {
    std::puts("palette_model: ok");
  }
  return failures == 0 ? 0 : 1;
}
