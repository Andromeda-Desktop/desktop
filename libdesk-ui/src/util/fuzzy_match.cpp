#include "util/fuzzy_match.h"


#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

namespace FuzzyMatch {

  namespace {

    // fzy (github.com/jhawthorn/fzy, MIT, third_party/fzy/LICENSE) over codepoints: its constants and programme.
    constexpr double kGapLeading = -0.005;
    constexpr double kGapTrailing = -0.005;
    constexpr double kGapInner = -0.01;
    constexpr double kConsecutive = 1.0;
    constexpr double kSlash = 0.9;
    constexpr double kWord = 0.8;
    constexpr double kCapital = 0.7;
    constexpr double kDot = 0.6;
    constexpr double kExact = 1024.0;
    constexpr std::size_t kMaxLen = 1024;

    char32_t fold(char32_t c) { return c >= U'A' && c <= U'Z' ? c + (U'a' - U'A') : c; }

    // Invalid bytes decode as themselves, so every byte stays one codepoint.
    void decode(std::string_view text, std::u32string& out) {
      out.clear();
      for (std::size_t i = 0; i < text.size();) {
        const auto b = static_cast<unsigned char>(text[i]);
        const std::size_t len = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 1;
        char32_t c = len == 1 ? b : b & (0x7FU >> len);
        std::size_t n = 1;
        for (; n < len && i + n < text.size() && (static_cast<unsigned char>(text[i + n]) & 0xC0) == 0x80; ++n) {
          c = (c << 6) | (static_cast<unsigned char>(text[i + n]) & 0x3F);
        }
        if (n < len) {
          c = b;
          n = 1;
        }
        i += n;
        out.push_back(c);
      }
    }

    double bonus(char32_t last, char32_t ch) {
      const bool upper = ch >= U'A' && ch <= U'Z';
      if (!upper && !(ch >= U'a' && ch <= U'z') && !(ch >= U'0' && ch <= U'9')) {
        return 0.0;
      }
      switch (last) {
      case U'/':
        return kSlash;
      case U'-':
      case U'_':
      case U' ':
        return kWord;
      case U'.':
        return kDot;
      default:
        return upper && last >= U'a' && last <= U'z' ? kCapital : 0.0;
      }
    }

    // Hot per keystroke over every launcher row: thread-local buffers, no allocation once warm.
    std::optional<Match> run(std::string_view pattern, std::string_view text, bool positions) {
      if (pattern.empty()) {
        return Match{};
      }
      thread_local std::u32string needle;
      thread_local std::u32string hay;
      thread_local std::vector<char32_t> low;
      thread_local std::vector<double> bonusAt;
      thread_local std::vector<double> d;
      thread_local std::vector<double> mm;
      decode(pattern, needle);
      decode(text, hay);
      std::ranges::transform(needle, needle.begin(), fold);
      low.resize(hay.size());
      std::ranges::transform(hay, low.begin(), fold);
      std::size_t at = 0;
      for (const char32_t c : needle) {
        while (at < low.size() && low[at] != c) {
          ++at;
        }
        if (at++ >= low.size()) {
          return std::nullopt;
        }
      }
      const std::size_t n = needle.size();
      const std::size_t m = hay.size();
      if (m > kMaxLen) {
        return std::nullopt;
      }
      Match out;
      if (n == m) {
        out.score = kExact;
        for (std::uint32_t i = 0; positions && i < n; ++i) {
          out.positions.push_back(i);
        }
        return out;
      }
      bonusAt.resize(m);
      char32_t last = U'/';
      for (std::size_t j = 0; j < m; ++j) {
        bonusAt[j] = bonus(last, hay[j]);
        last = hay[j];
      }
      // d: best score ending in a match at (i, j); mm: best score at (i, j). One row each unless positions backtrack.
      const std::size_t rows = positions ? n : 1;
      d.assign(rows * m, noMatchScore);
      mm.assign(rows * m, noMatchScore);
      for (std::size_t i = 0; i < n; ++i) {
        double* currD = &d[positions ? i * m : 0];
        double* currM = &mm[positions ? i * m : 0];
        const double* lastD = positions && i > 0 ? currD - m : currD;
        const double* lastM = positions && i > 0 ? currM - m : currM;
        const double gap = i == n - 1 ? kGapTrailing : kGapInner;
        double prevScore = noMatchScore;
        double prevM = noMatchScore;
        double prevD = noMatchScore;
        for (std::size_t j = 0; j < m; ++j) {
          double score = noMatchScore;
          if (needle[i] == low[j]) {
            if (i == 0) {
              score = static_cast<double>(j) * kGapLeading + bonusAt[j];
            } else if (j > 0) {
              score = std::max(prevM + bonusAt[j], prevD + kConsecutive);
            }
          }
          prevD = lastD[j];
          prevM = lastM[j];
          currD[j] = score;
          currM[j] = prevScore = std::max(score, prevScore + gap);
        }
      }
      out.score = mm[(rows - 1) * m + m - 1];
      if (!std::isfinite(out.score)) {
        return std::nullopt;
      }
      if (positions) {
        // Of equal paths, the latest in the text; a consecutive score needs the previous letter matched.
        out.positions.resize(n);
        bool required = false;
        std::ptrdiff_t j = static_cast<std::ptrdiff_t>(m) - 1;
        for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(n) - 1; i >= 0; --i) {
          for (; j >= 0; --j) {
            const auto cell = static_cast<std::size_t>(i) * m + static_cast<std::size_t>(j);
            if (d[cell] != noMatchScore && (required || d[cell] == mm[cell])) {
              required = i > 0 && j > 0 && mm[cell] == d[cell - m - 1] + kConsecutive;
              out.positions[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(j--);
              break;
            }
          }
        }
      }
      return out;
    }

  } // namespace

  double score(std::string_view pattern, std::string_view text) {
    const auto found = run(pattern, text, false);
    return found ? found->score : noMatchScore;
  }

  std::optional<Match> match(std::string_view pattern, std::string_view text) { return run(pattern, text, true); }

  std::optional<int> typoDistance(std::string_view pattern, std::string_view text) {
    const int allowed = pattern.size() < 5 ? 0 : (pattern.size() < 7 ? 1 : 2);
    if (allowed == 0) {
      return std::nullopt;
    }
    // Hot per keystroke over every app: fold both strings once and reuse one table; no allocation per word start.
    const auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; };
    const auto alnum = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
    thread_local std::string p;
    thread_local std::string t;
    thread_local std::vector<int> d;
    p.assign(pattern);
    t.assign(text);
    std::ranges::transform(p, p.begin(), fold);
    std::ranges::transform(t, t.begin(), fold);
    const std::size_t m = p.size();
    std::optional<int> best;

    for (std::size_t start = 0; start < t.size(); ++start) {
      const bool wordStart = start == 0 || !alnum(t[start - 1]);
      if (!wordStart || !alnum(t[start])) {
        continue;
      }
      const std::string_view word = std::string_view(t).substr(start, std::min(t.size() - start, m + static_cast<std::size_t>(allowed)));
      const std::size_t n = word.size();
      // Optimal string alignment table; the answer is the best prefix of the word, so the last row's minimum.
      d.assign((m + 1) * (n + 1), 0);
      const auto at = [&](std::size_t i, std::size_t j) -> int& { return d[i * (n + 1) + j]; };
      for (std::size_t i = 0; i <= m; ++i) {
        at(i, 0) = static_cast<int>(i);
      }
      for (std::size_t j = 0; j <= n; ++j) {
        at(0, j) = static_cast<int>(j);
      }
      for (std::size_t i = 1; i <= m; ++i) {
        for (std::size_t j = 1; j <= n; ++j) {
          const int cost = p[i - 1] == word[j - 1] ? 0 : 1;
          at(i, j) = std::min({at(i - 1, j) + 1, at(i, j - 1) + 1, at(i - 1, j - 1) + cost});
          if (i > 1 && j > 1 && p[i - 1] == word[j - 2] && p[i - 2] == word[j - 1]) {
            at(i, j) = std::min(at(i, j), at(i - 2, j - 2) + 1);
          }
        }
      }
      int distance = at(m, 0);
      for (std::size_t j = 1; j <= n; ++j) {
        distance = std::min(distance, at(m, j));
      }
      if (distance <= allowed && (!best.has_value() || distance < *best)) {
        best = distance;
        if (distance == 0) {
          break;
        }
      }
    }
    return best;
  }

} // namespace FuzzyMatch
