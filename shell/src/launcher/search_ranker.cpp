#include "launcher/search_ranker.h"

#include "util/fuzzy_match.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {

  constexpr double kUsageScorePerCount = 0.1;
  constexpr double kTypedUsageScoreCap = 0.5;
  constexpr double kHalfLifeDays = 7.0;
  constexpr double kSecondsPerDay = 86400.0;

} // namespace

namespace search_ranker {

  double frecency(int usageCount, std::optional<std::int64_t> lastUsedSeconds, std::int64_t nowSeconds) {
    if (usageCount <= 0) {
      return 0.0;
    }
    if (!lastUsedSeconds.has_value()) {
      return usageCount;
    }
    const double days = static_cast<double>(std::max<std::int64_t>(0, nowSeconds - *lastUsedSeconds)) / kSecondsPerDay;
    return usageCount * std::pow(0.5, days / kHalfLifeDays);
  }

  double usageBoost(double score, double frecency, bool typedQuery) {
    if (frecency <= 0.0) {
      return 0.0;
    }
    const double rawBoost = frecency * kUsageScorePerCount;
    if (!typedQuery) {
      return rawBoost;
    }
    if (!FuzzyMatch::isMatch(score)) {
      return 0.0;
    }
    return std::min(rawBoost, kTypedUsageScoreCap);
  }

  void sort(std::vector<LauncherResult>& results, const std::vector<std::string>& demotedProviders) {
    const auto demoted = [&](const LauncherResult& result) {
      return std::ranges::find(demotedProviders, result.providerId) != demotedProviders.end();
    };
    // Sort small keys, then move each result at most once: a result is large, and a stable sort of
    // thousands (the full emoji grid) moved each one log2(n) times.
    struct Key {
      bool demoted;
      double score;
      std::size_t index;
    };
    std::vector<Key> keys;
    keys.reserve(results.size());
    for (std::size_t i = 0; i < results.size(); ++i) {
      keys.push_back({demoted(results[i]), results[i].score, i});
    }
    std::ranges::stable_sort(keys, [](const Key& a, const Key& b) {
      if (a.demoted != b.demoted) {
        return b.demoted;
      }
      return a.score > b.score;
    });
    const auto moved = [&](const Key& k) { return k.index != static_cast<std::size_t>(&k - keys.data()); };
    if (std::ranges::none_of(keys, moved)) {
      return;
    }
    std::vector<LauncherResult> sorted;
    sorted.reserve(results.size());
    for (const Key& key : keys) {
      sorted.push_back(std::move(results[key.index]));
    }
    results = std::move(sorted);
  }

} // namespace search_ranker
