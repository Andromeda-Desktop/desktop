#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace FuzzyMatch {

  inline constexpr double noMatchScore = -std::numeric_limits<double>::infinity();

  [[nodiscard]] inline bool isMatch(double value) noexcept { return value > noMatchScore; }

  struct Match {
    double score = 0.0;
    std::vector<std::uint32_t> positions; // ascending codepoint indices into the text
  };

  // fzy ranking over UTF-8 codepoints, ASCII case folded: 1024 for an exact match, noMatchScore when the pattern is not
  // a subsequence of the text or the text exceeds 1024 codepoints; 0 for an empty pattern.
  [[nodiscard]] double score(std::string_view pattern, std::string_view text);
  // score() with the matched codepoints, for highlights; nullopt where score() is noMatchScore.
  [[nodiscard]] std::optional<Match> match(std::string_view pattern, std::string_view text);

  // Fewest edits (insert, delete, change, swap of neighbours) that turn the pattern into the start of the text or of a
  // word in it, ASCII case folded; nullopt above the allowance: no typo under 5 letters, 1 from 5, 2 from 7. After
  // nanolight (github.com/justrach/nanolight, MIT): "chorme" finds Chrome.
  [[nodiscard]] std::optional<int> typoDistance(std::string_view pattern, std::string_view text);

} // namespace FuzzyMatch
