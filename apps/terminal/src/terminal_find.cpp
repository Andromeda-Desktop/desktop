#include "terminal_find.h"

#include <algorithm>
#include <span>

namespace term {

  TerminalFind::~TerminalFind() { ghostty_search_free(m_search); }

  void TerminalFind::setQuery(GhosttyTerminal terminal, std::string_view query) {
    if (terminal == nullptr || (m_search == nullptr && ghostty_search_new(nullptr, &m_search, terminal) != GHOSTTY_SUCCESS)) {
      return;
    }
    const GhosttyString needle{.ptr = reinterpret_cast<const std::uint8_t*>(query.data()), .len = query.size()};
    ghostty_search_set(m_search, GHOSTTY_SEARCH_OPT_NEEDLE, &needle);
    m_selectFirst = !query.empty();
    m_reported = false;
    m_running = true;
  }

  void TerminalFind::step(bool older) {
    if (m_search == nullptr) {
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
    if (m_search == nullptr) {
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

  bool TerminalFind::hitAt(std::uint32_t row, std::uint16_t col, bool& current) const {
    const auto in = [&](const FindHit& h) {
      const bool afterStart = row > h.start.y || (row == h.start.y && col >= h.start.x);
      const bool beforeEnd = row < h.end.y || (row == h.end.y && col <= h.end.x);
      return afterStart && beforeEnd;
    };
    current = m_current && in(*m_current);
    return current || std::ranges::any_of(m_hits, in);
  }

} // namespace term
