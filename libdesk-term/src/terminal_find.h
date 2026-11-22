#pragma once

#include <ghostty/vt.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace term {

  struct FindHit {
    GhosttyPointCoordinate start; // viewport cells, inclusive
    GhosttyPointCoordinate end;
    bool operator==(const FindHit& o) const {
      return start.x == o.start.x && start.y == o.start.y && end.x == o.end.x && end.y == o.end.y;
    }
  };

  struct FindOptions {
    bool regex = false;
    bool matchCase = false;
    bool operator==(const FindOptions&) const = default;
  };

  enum class FindError : std::uint8_t { None, InvalidPattern, TooComplex };

  // Every call except tick() runs under the terminal lock, and so does the destructor.
  // A plain query uses libghostty's incremental search (ASCII case-insensitive); a regex or case-sensitive query
  // matches the formatted screen text itself.
  class TerminalFind {
  public:
    TerminalFind() = default;
    ~TerminalFind();
    TerminalFind(TerminalFind&&) = delete;
    TerminalFind& operator=(TerminalFind&&) = delete;
    TerminalFind(const TerminalFind&) = delete;
    TerminalFind& operator=(const TerminalFind&) = delete;

    void setQuery(GhosttyTerminal terminal, std::string_view query, FindOptions options = {});
    void step(bool older);
    // Feeds the search and reads the results; true when total, index or marks changed, and on the first read of a query.
    bool read(GhosttyTerminal terminal);
    // Bounded search progress off the lock; true while another read() is needed.
    bool tick(std::chrono::nanoseconds budget);

    [[nodiscard]] const std::vector<FindHit>& hits() const { return m_hits; }
    [[nodiscard]] const std::optional<FindHit>& current() const { return m_current; }
    [[nodiscard]] std::size_t total() const { return m_total; }
    [[nodiscard]] std::optional<std::size_t> index() const { return m_index; } // 0 = newest
    [[nodiscard]] const std::vector<float>& marks() const { return m_marks; }  // scrollbar fractions
    [[nodiscard]] std::optional<float> currentMark() const { return m_currentMark; }
    [[nodiscard]] bool hitAt(std::uint32_t row, std::uint16_t col, bool& current) const;
    [[nodiscard]] FindError error() const { return m_error; }
    // The own matcher runs: it sees new screen text only after contentChanged(), at the next read().
    [[nodiscard]] bool scansText() const { return m_text.has_value(); }
    void contentChanged();

  private:
    // A match in screen rows, by byte offset into each row's formatted text; end is exclusive.
    struct TextHit {
      std::uint32_t startRow = 0;
      std::uint32_t startByte = 0;
      std::uint32_t endRow = 0;
      std::uint32_t endByte = 0;
      auto operator<=>(const TextHit&) const = default;
    };
    struct TextQuery {
      std::string needle;
      FindOptions options;
    };
    bool readText(GhosttyTerminal terminal);
    void scanText(GhosttyTerminal terminal);
    void viewportHits(GhosttyTerminal terminal, const GhosttyTerminalScrollbar& scroll);

    std::optional<TextQuery> m_text;     // set while the own matcher runs instead of libghostty's search
    std::vector<TextHit> m_textHits;     // top of the screen first
    std::optional<std::size_t> m_textSel; // into m_textHits
    enum class TextScan : std::uint8_t { Fresh, ScreenChanged, QueryChanged };
    TextScan m_textScan = TextScan::Fresh;
    bool m_textReveal = false;           // scroll the selection into view at the next read
    bool m_textViewStale = false;        // m_hits must be rebuilt from m_textHits
    std::uint64_t m_textOffset = 0;      // viewport offset m_hits was built for
    FindError m_error = FindError::None;

    GhosttySearch m_search = nullptr;
    bool m_selectFirst = false; // select the newest match once one is found
    std::vector<FindHit> m_hits; // in the viewport, sorted by start
    std::uint32_t m_maxSpan = 0; // most rows one hit spans past its start
    std::optional<FindHit> m_current;
    std::size_t m_total = 0;
    std::optional<std::size_t> m_index;
    std::vector<float> m_marks;
    std::optional<float> m_currentMark;
    bool m_reported = false;     // total and index reached the caller since the query changed
    bool m_running = false;      // the last tick left the search unfinished
    bool m_marksStale = false;
    std::uint64_t m_marksTotal = 0; // scroll total the marks are fractions of
    std::vector<GhosttySelection> m_scratch;
  };

} // namespace term
