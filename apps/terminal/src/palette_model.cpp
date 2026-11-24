#include "palette_model.h"

#include "util/fuzzy_match.h"

#include <algorithm>

namespace term::palette {

  namespace {

    char32_t lower(char32_t c) { return c >= U'A' && c <= U'Z' ? c + (U'a' - U'A') : c; }

  } // namespace

  std::optional<Fuzzy> fuzzy(std::string_view query, std::string_view text) {
    std::string pattern(query);
    std::erase_if(pattern, [](char c) { return c == ' ' || c == '\t'; });
    auto found = FuzzyMatch::match(pattern, text);
    if (!found) {
      return std::nullopt;
    }
    return Fuzzy{static_cast<float>(found->score), std::move(found->positions)};
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
