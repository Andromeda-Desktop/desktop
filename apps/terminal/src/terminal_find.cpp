#include "terminal_find.h"

#include "util/safe_regex.h"

#include <algorithm>
#include <span>

namespace term {

  TerminalFind::~TerminalFind() { ghostty_search_free(m_search); }

  void TerminalFind::setQuery(GhosttyTerminal terminal, std::string_view query, FindOptions options) {
    if (terminal == nullptr || (m_search == nullptr && ghostty_search_new(nullptr, &m_search, terminal) != GHOSTTY_SUCCESS)) {
      return;
    }
    m_error = FindError::None;
    const bool own = options != FindOptions{} && !query.empty();
    if (own) {
      m_text = TextQuery{.needle = std::string(query), .options = options};
      m_textScan = TextScan::QueryChanged;
      m_textSel.reset();
      query = {}; // libghostty's search stops
    } else {
      m_text.reset();
      m_textHits.clear();
      m_textSel.reset();
    }
    const GhosttyString needle{.ptr = reinterpret_cast<const std::uint8_t*>(query.data()), .len = query.size()};
    ghostty_search_set(m_search, GHOSTTY_SEARCH_OPT_NEEDLE, &needle);
    m_selectFirst = own || !query.empty();
    m_reported = false;
    m_running = !own;
  }

  void TerminalFind::step(bool older) {
    if (m_search == nullptr) {
      return;
    }
    if (m_text) {
      const std::size_t n = m_textHits.size();
      if (n == 0) {
        return;
      }
      m_textSel = !m_textSel ? n - 1 : older ? (*m_textSel + n - 1) % n : (*m_textSel + 1) % n;
      m_selectFirst = false;
      m_textReveal = true;
      m_textViewStale = true;
      return;
    }
    if (older) {
      ghostty_search_select_next(m_search);
    } else {
      ghostty_search_select_prev(m_search);
    }
    std::size_t index = 0;
    m_selectFirst = m_selectFirst && ghostty_search_get(m_search, GHOSTTY_SEARCH_DATA_SELECTED_INDEX, &index) != GHOSTTY_SUCCESS;
  }

  bool TerminalFind::read(GhosttyTerminal t) {
    if (m_search == nullptr) {
      return false;
    }
    if (m_text) {
      return readText(t);
    }
    ghostty_search_feed(m_search);
    std::size_t total = 0;
    ghostty_search_get(m_search, GHOSTTY_SEARCH_DATA_TOTAL_MATCHES, &total);
    if (m_selectFirst && total > 0) {
      ghostty_search_select_next(m_search);
      m_selectFirst = false;
      ghostty_search_feed(m_search); // the viewport may have moved to the match
    }
    GhosttyTerminalScrollbar scroll{};
    ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &scroll);
    std::size_t index = 0;
    const bool selected = ghostty_search_get(m_search, GHOSTTY_SEARCH_DATA_SELECTED_INDEX, &index) == GHOSTTY_SUCCESS;
    const auto point = [&](const GhosttyGridRef& ref, GhosttyPointTag tag) -> std::optional<GhosttyPointCoordinate> {
      GhosttyPointCoordinate at{};
      if (ghostty_terminal_point_from_grid_ref(t, &ref, tag, &at) != GHOSTTY_SUCCESS) {
        return std::nullopt;
      }
      return at;
    };
    const auto before = [](const GhosttyPointCoordinate& a, const GhosttyPointCoordinate& b) {
      return a.y < b.y || (a.y == b.y && a.x < b.x);
    };
    // The ends keep the order they were found in; hits are stored top-left first.
    // Viewport matches come a page at a time, so rows past the viewport are dropped here.
    const auto hit = [&](const GhosttySelection& sel) -> std::optional<FindHit> {
      const auto a = point(sel.start, GHOSTTY_POINT_TAG_VIEWPORT);
      const auto b = point(sel.end, GHOSTTY_POINT_TAG_VIEWPORT);
      if (!a || !b) {
        return std::nullopt;
      }
      const FindHit h = before(*b, *a) ? FindHit{*b, *a} : FindHit{*a, *b};
      return h.start.y < scroll.len ? std::optional<FindHit>(h) : std::nullopt;
    };
    const auto matches = [&](GhosttySearchData key) {
      GhosttySelectionBuffer buffer{.ptr = nullptr, .cap = 0, .len = 0};
      if (ghostty_search_get(m_search, key, &buffer) == GHOSTTY_OUT_OF_SPACE) {
        m_scratch.resize(buffer.len);
        buffer = {.ptr = m_scratch.data(), .cap = m_scratch.size(), .len = 0};
        ghostty_search_get(m_search, key, &buffer);
      }
      return std::span<const GhosttySelection>(m_scratch.data(), buffer.ptr != nullptr ? buffer.len : 0);
    };
    m_hits.clear();
    for (const GhosttySelection& sel : matches(GHOSTTY_SEARCH_DATA_VIEWPORT_MATCHES)) {
      if (const auto h = hit(sel)) {
        m_hits.push_back(*h);
      }
    }
    std::ranges::sort(m_hits, before, &FindHit::start);
    m_maxSpan = 0;
    for (const FindHit& h : m_hits) {
      m_maxSpan = std::max(m_maxSpan, h.end.y - h.start.y);
    }
    GhosttySelection current{};
    const bool hasCurrent =
        selected && ghostty_search_get(m_search, GHOSTTY_SEARCH_DATA_SELECTED_MATCH, &current) == GHOSTTY_SUCCESS;
    m_current = hasCurrent ? hit(current) : std::nullopt;
    const auto newIndex = selected ? std::optional<std::size_t>(index) : std::nullopt;
    const bool changed = total != m_total || newIndex != m_index || !m_reported;
    m_total = total;
    m_index = newIndex;
    m_reported = true;
    m_marksStale = m_marksStale || changed || scroll.total != m_marksTotal;
    // Copying every match each frame of a long search is costly: marks wait until it completes.
    if (!m_marksStale || m_running) {
      return changed;
    }
    m_marksStale = false;
    m_marksTotal = scroll.total;
    // Scrollbar marks: one per track step is enough, so many hits stay cheap.
    constexpr std::size_t kSteps = 1024;
    m_marks.clear();
    m_currentMark.reset();
    if (scroll.total > 0) {
      const auto fraction = [&](std::uint32_t row) { return static_cast<float>(row) / static_cast<float>(scroll.total); };
      std::size_t last = kSteps;
      for (const GhosttySelection& sel : matches(GHOSTTY_SEARCH_DATA_MATCHES)) {
        if (const auto at = point(sel.start, GHOSTTY_POINT_TAG_SCREEN)) {
          const auto step = static_cast<std::size_t>(fraction(at->y) * static_cast<float>(kSteps - 1));
          if (step != last) {
            m_marks.push_back(fraction(at->y));
            last = step;
          }
        }
      }
      const auto at = hasCurrent ? point(current.start, GHOSTTY_POINT_TAG_SCREEN) : std::nullopt;
      m_currentMark = at ? std::optional<float>(fraction(at->y)) : std::nullopt;
    }
    return true;
  }

  bool TerminalFind::tick(std::chrono::nanoseconds budget) {
    if (m_search == nullptr || m_text) {
      return false;
    }
    GhosttySearchStatus status = GHOSTTY_SEARCH_STATUS_COMPLETE;
    const auto until = std::chrono::steady_clock::now() + budget;
    while (ghostty_search_tick(m_search, &status) == GHOSTTY_SUCCESS && status == GHOSTTY_SEARCH_STATUS_RUNNING
           && std::chrono::steady_clock::now() < until) {
    }
    m_running = status != GHOSTTY_SEARCH_STATUS_COMPLETE;
    // Matches found by these ticks reach the view only through the next read().
    std::size_t total = 0;
    ghostty_search_get(m_search, GHOSTTY_SEARCH_DATA_TOTAL_MATCHES, &total);
    return m_running || m_marksStale || total != m_total || (m_selectFirst && total > 0);
  }

  void TerminalFind::contentChanged() {
    if (m_text && m_textScan == TextScan::Fresh) {
      m_textScan = TextScan::ScreenChanged;
    }
  }

  namespace {
    std::string formatScreen(GhosttyTerminal t, bool unwrap) {
      GhosttyFormatter formatter = nullptr;
      if (ghostty_formatter_terminal_new(nullptr, &formatter, t) != GHOSTTY_SUCCESS) {
        return {};
      }
      const bool trim = false;
      ghostty_formatter_set(formatter, GHOSTTY_FORMATTER_OPT_UNWRAP, &unwrap);
      ghostty_formatter_set(formatter, GHOSTTY_FORMATTER_OPT_TRIM, &trim);
      std::uint8_t* ptr = nullptr;
      std::size_t len = 0;
      std::string out;
      if (ghostty_formatter_format_alloc(formatter, nullptr, &ptr, &len) == GHOSTTY_SUCCESS) {
        out.assign(reinterpret_cast<const char*>(ptr), len);
        ghostty_free(nullptr, ptr, len);
      }
      ghostty_formatter_free(formatter);
      return out;
    }

    std::size_t utf8Length(std::uint32_t cp) {
      return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    }
  } // namespace

  // Plain output without trimming has one line per screen row (a blank cell is one space, a wide cell's spacer
  // nothing); unwrapped, the rows of a soft-wrapped line are the same bytes joined. Matches run per unwrapped line.
  void TerminalFind::scanText(GhosttyTerminal t) {
    const auto previous = m_textSel ? std::optional<TextHit>(m_textHits[*m_textSel]) : std::nullopt;
    m_textHits.clear();
    m_textSel.reset();
    m_textViewStale = true;
    m_error = FindError::None;
    std::optional<SafeRegex> regex;
    if (m_text->options.regex) {
      regex = SafeRegex::compile(m_text->needle, !m_text->options.matchCase);
      if (!regex) {
        m_error = FindError::InvalidPattern;
        return;
      }
    }
    const std::string rows = formatScreen(t, false);
    const std::string lines = formatScreen(t, true);
    struct Segment {
      std::uint32_t row;
      std::size_t at; // offset of the row in its line
    };
    std::vector<Segment> segments;
    std::size_t rowPos = 0;
    std::uint32_t row = 0;
    for (std::size_t linePos = 0; linePos <= lines.size();) {
      const std::size_t lineEnd = std::min(lines.find('\n', linePos), lines.size());
      const std::string_view line(lines.data() + linePos, lineEnd - linePos);
      segments.clear();
      std::size_t length = 0;
      do {
        const std::size_t rowEnd = std::min(rows.find('\n', rowPos), rows.size());
        segments.push_back({row++, length});
        length += rowEnd - rowPos;
        rowPos = rowEnd + 1;
      } while (length < line.size() && rowPos <= rows.size());
      if (length != line.size()) {
        break; // the two layouts disagree; matches past here would land on the wrong rows
      }
      const auto at = [&](std::size_t offset) {
        const auto seg = std::ranges::upper_bound(segments, offset, {}, &Segment::at) - 1;
        return std::pair{seg->row, static_cast<std::uint32_t>(offset - seg->at)};
      };
      const auto found = [&](std::size_t start, std::size_t end) {
        const auto [startRow, startByte] = at(start);
        const auto [endRow, endByte] = at(end - 1);
        m_textHits.push_back({.startRow = startRow, .startByte = startByte, .endRow = endRow, .endByte = endByte});
      };
      if (regex) {
        if (!regex->forEachMatch(line, found) && regex->exhausted()) {
          m_error = FindError::TooComplex;
          m_textHits.clear();
          return;
        }
      } else {
        for (std::size_t pos = line.find(m_text->needle); pos != std::string_view::npos;
             pos = line.find(m_text->needle, pos + m_text->needle.size())) {
          found(pos, pos + m_text->needle.size());
        }
      }
      if (lineEnd == lines.size()) {
        break;
      }
      linePos = lineEnd + 1;
    }
    if (previous && !m_textHits.empty()) {
      const auto it = std::ranges::lower_bound(m_textHits, *previous);
      m_textSel = static_cast<std::size_t>(std::min(it - m_textHits.begin(), std::ssize(m_textHits) - 1));
    }
  }

  void TerminalFind::viewportHits(GhosttyTerminal t, const GhosttyTerminalScrollbar& scroll) {
    m_textViewStale = false;
    m_textOffset = scroll.offset;
    m_hits.clear();
    m_current.reset();
    m_maxSpan = 0;
    std::uint16_t cols = 0;
    ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_COLS, &cols);
    constexpr std::uint32_t kNone = UINT32_MAX;
    std::vector<std::vector<std::uint32_t>> starts(scroll.len); // per viewport row: first byte of each cell
    std::vector<std::uint32_t> codepoints(8);
    const auto column = [&](std::uint32_t y, std::uint32_t byte) -> std::uint16_t {
      auto& row = starts[y];
      if (row.empty()) {
        row.assign(cols, kNone);
        std::uint32_t at = 0;
        for (std::uint16_t x = 0; x < cols; ++x) {
          GhosttyGridRef ref{};
          GhosttyCell cell = 0;
          const GhosttyPoint point{.tag = GHOSTTY_POINT_TAG_VIEWPORT, .value = {.coordinate = {.x = x, .y = y}}};
          if (ghostty_terminal_grid_ref(t, point, &ref) != GHOSTTY_SUCCESS || ghostty_grid_ref_cell(&ref, &cell) != GHOSTTY_SUCCESS) {
            break;
          }
          GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
          ghostty_cell_get(cell, GHOSTTY_CELL_DATA_WIDE, &wide);
          if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL || wide == GHOSTTY_CELL_WIDE_SPACER_HEAD) {
            continue;
          }
          std::size_t count = 0;
          if (ghostty_grid_ref_graphemes(&ref, codepoints.data(), codepoints.size(), &count) == GHOSTTY_OUT_OF_SPACE) {
            codepoints.resize(count);
            ghostty_grid_ref_graphemes(&ref, codepoints.data(), codepoints.size(), &count);
          }
          std::size_t bytes = count == 0 ? 1 : 0; // an empty cell is a space
          for (std::size_t i = 0; i < count; ++i) {
            bytes += utf8Length(codepoints[i]);
          }
          row[x] = at;
          at += static_cast<std::uint32_t>(bytes);
        }
      }
      std::uint16_t best = 0;
      for (std::uint16_t x = 0; x < cols && (row[x] == kNone || row[x] <= byte); ++x) {
        best = row[x] != kNone ? x : best;
      }
      return best;
    };
    const std::uint64_t top = scroll.offset;
    const std::uint64_t bottom = scroll.offset + scroll.len;
    const auto first = std::ranges::lower_bound(m_textHits, top, {}, &TextHit::startRow);
    for (auto it = first; it != m_textHits.end() && it->startRow < bottom; ++it) {
      const auto y0 = static_cast<std::uint32_t>(it->startRow - top);
      const auto y1 = static_cast<std::uint32_t>(it->endRow - top);
      const FindHit h{.start = {.x = column(y0, it->startByte), .y = y0},
                      .end = {.x = it->endRow < bottom ? column(y1, it->endByte) : static_cast<std::uint16_t>(cols - 1), .y = y1}};
      m_hits.push_back(h);
      m_maxSpan = std::max(m_maxSpan, y1 - y0);
      if (m_textSel && it - m_textHits.begin() == static_cast<std::ptrdiff_t>(*m_textSel)) {
        m_current = h;
      }
    }
  }

  bool TerminalFind::readText(GhosttyTerminal t) {
    const bool scan = m_textScan != TextScan::Fresh;
    if (scan) {
      scanText(t);
      m_textScan = TextScan::Fresh;
    }
    const std::size_t total = m_textHits.size();
    if (m_selectFirst && total > 0) {
      m_textSel = total - 1;
      m_selectFirst = false;
      m_textReveal = true;
    }
    GhosttyTerminalScrollbar scroll{};
    ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &scroll);
    if (m_textReveal && m_textSel) {
      m_textReveal = false;
      const std::uint64_t row = m_textHits[*m_textSel].startRow;
      if (row < scroll.offset || row >= scroll.offset + scroll.len) {
        const GhosttyTerminalScrollViewport to{.tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_ROW,
                                               .value = {.row = row > scroll.len / 2 ? row - scroll.len / 2 : 0}};
        ghostty_terminal_scroll_viewport(t, to);
        ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &scroll);
      }
    }
    if (m_textViewStale || scroll.offset != m_textOffset) {
      viewportHits(t, scroll);
    }
    const auto index = m_textSel ? std::optional<std::size_t>(total - 1 - *m_textSel) : std::nullopt;
    const bool changed = total != m_total || index != m_index || !m_reported;
    m_total = total;
    m_index = index;
    m_reported = true;
    if (!scan && !changed && scroll.total == m_marksTotal) {
      return false;
    }
    m_marksTotal = scroll.total;
    m_marks.clear();
    m_currentMark.reset();
    if (scroll.total > 0) {
      constexpr std::size_t kSteps = 1024;
      const auto fraction = [&](std::uint32_t r) { return static_cast<float>(r) / static_cast<float>(scroll.total); };
      std::size_t last = kSteps;
      for (const TextHit& h : m_textHits) {
        const auto step = static_cast<std::size_t>(fraction(h.startRow) * static_cast<float>(kSteps - 1));
        if (step != last) {
          m_marks.push_back(fraction(h.startRow));
          last = step;
        }
      }
      if (m_textSel) {
        m_currentMark = fraction(m_textHits[*m_textSel].startRow);
      }
    }
    return true;
  }

  bool TerminalFind::hitAt(std::uint32_t row, std::uint16_t col, bool& current) const {
    const auto in = [&](const FindHit& h) {
      const bool afterStart = row > h.start.y || (row == h.start.y && col >= h.start.x);
      const bool beforeEnd = row < h.end.y || (row == h.end.y && col <= h.end.x);
      return afterStart && beforeEnd;
    };
    current = m_current && in(*m_current);
    if (current) {
      return true;
    }
    // Hits are sorted by start: only those starting from row - m_maxSpan up to this cell can cover it.
    const std::uint32_t first = row > m_maxSpan ? row - m_maxSpan : 0;
    for (auto it = std::ranges::lower_bound(m_hits, first, {}, [](const FindHit& h) { return h.start.y; });
         it != m_hits.end() && (it->start.y < row || (it->start.y == row && it->start.x <= col)); ++it) {
      if (in(*it)) {
        return true;
      }
    }
    return false;
  }

} // namespace term
