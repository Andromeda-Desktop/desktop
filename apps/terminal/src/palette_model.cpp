#include "palette_model.h"

#include <algorithm>

namespace term::palette {

  namespace {

    char32_t lower(char32_t c) { return c >= U'A' && c <= U'Z' ? c + (U'a' - U'A') : c; }

    // Invalid bytes decode as themselves, so every byte stays one codepoint.
    std::u32string decode(std::string_view text, bool dropSpaces) {
      std::u32string out;
      out.reserve(text.size());
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
        if (!(dropSpaces && (c == U' ' || c == U'\t'))) {
          out.push_back(lower(c));
        }
      }
      return out;
    }

    bool wordStart(const std::u32string& t, std::size_t i) {
      if (i == 0) {
        return true;
      }
      const char32_t p = t[i - 1];
      return p == U' ' || p == U'/' || p == U'-' || p == U'_' || p == U'.';
    }

  } // namespace

  std::optional<Fuzzy> fuzzy(std::string_view query, std::string_view text) {
    const std::u32string q = decode(query, true);
    if (q.empty()) {
      return Fuzzy{};
    }
    const std::u32string t = decode(text, false);
    std::optional<Fuzzy> best;
    std::vector<std::uint32_t> hits;
    for (std::size_t start = 0; start < t.size(); ++start) {
      if (t[start] != q[0]) {
        continue;
      }
      hits.clear();
      float score = 0.0F;
      std::size_t ti = start;
      for (const char32_t ch : q) {
        while (ti < t.size() && t[ti] != ch) {
          ++ti;
        }
        if (ti >= t.size()) {
          break;
        }
        if (!hits.empty() && ti == hits.back() + 1) {
          score += 3.0F;
        }
        if (wordStart(t, ti)) {
          score += 2.0F;
        }
        hits.push_back(static_cast<std::uint32_t>(ti++));
      }
      if (hits.size() != q.size()) {
        break; // a later start cannot find the letters an earlier one missed
      }
      score -= static_cast<float>(start) * 0.01F;
      if (!best || score > best->score) {
        best = Fuzzy{score, hits};
      }
    }
    return best;
  }

  std::vector<Match> rank(std::string_view query, std::span<const Item> items) {
    constexpr std::string_view kThemePrefix = "theme";
    bool themesOnly = false;
    if (query.size() >= kThemePrefix.size() &&
        std::ranges::equal(query.substr(0, kThemePrefix.size()), kThemePrefix, [](char a, char b) {
          return lower(static_cast<unsigned char>(a)) == static_cast<char32_t>(b);
        })) {
      themesOnly = true;
      query.remove_prefix(kThemePrefix.size());
    }
    std::vector<Match> out;
    for (std::size_t i = 0; i < items.size(); ++i) {
      const Item& item = items[i];
      if (themesOnly && item.kind != Kind::Theme) {
        continue;
      }
      if (auto m = fuzzy(query, item.label)) {
        out.push_back({.item = i, .score = m->score, .hits = std::move(m->hits)});
      } else if (auto s = item.sub.empty() ? std::nullopt : fuzzy(query, item.sub)) {
        out.push_back({.item = i, .score = s->score - 1.0F, .subHits = std::move(s->hits)});
      }
    }
    std::ranges::stable_sort(out, [&](const Match& a, const Match& b) {
      const bool aOff = !items[a.item].off.empty();
      const bool bOff = !items[b.item].off.empty();
      return aOff != bOff ? bOff : a.score > b.score;
    });
    return out;
  }

} // namespace term::palette
