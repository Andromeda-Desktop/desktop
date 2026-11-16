#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The command palette's rows and its fuzzy ranking (Ctrl+Shift+P). No UI.
namespace term::palette {

  enum class Kind : std::uint8_t { Session, Action, Theme };

  struct Item {
    Kind kind = Kind::Action;
    std::string id; // stable across opens: "session:<id>", "action:<name>", "theme:<id>"
    std::string label;
    std::string sub;                       // a session's status or folder; matched after the label
    std::string trailing;                  // "this pane", "window 2", "current"; shown when the
                                           // row has no keys
    std::vector<std::string> keys;         // keycap texts of the bound chord
    std::string off;                       // why the row cannot run now; empty: it can
    std::string glyph;                     // symbol name; a theme row shows its swatch instead
    bool danger = false;                   // the symbol in danger_text (a failed command)
    std::array<std::uint32_t, 3> swatch{}; // a theme's surface, accent and one ANSI colour, 0xRRGGBBAA
  };

  struct Fuzzy {
    float score = 0.0F;
    std::vector<std::uint32_t> hits; // codepoint indexes of the matched letters
  };

  struct Match {
    std::size_t item = 0;
    float score = 0.0F;
    std::vector<std::uint32_t> hits;    // in the label
    std::vector<std::uint32_t> subHits; // in the sub, when only the sub matched
  };

  // Subsequence match, case-blind for ASCII; spaces in the query are ignored.
  // Consecutive letters and word starts score higher, a later first letter a
  // little lower. nullopt: no match.
  [[nodiscard]] std::optional<Fuzzy> fuzzy(std::string_view query, std::string_view text);

  // Usable rows first, then rows that cannot run; each group by score, best
  // first, ties in item order. A query that starts with "theme" lists only themes
  // and matches the rest of it.
  [[nodiscard]] std::vector<Match> rank(std::string_view query, std::span<const Item> items);

} // namespace term::palette
