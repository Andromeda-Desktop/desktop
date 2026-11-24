#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace viewer {

  struct RectF {
    float x0 = 0.0F;
    float y0 = 0.0F;
    float x1 = 0.0F;
    float y1 = 0.0F;
  };

  // Page geometry, zoom and search state of one open PDF; no I/O, no rendering.
  class DocumentModel {
  public:
    static constexpr float kPageGap = 12.0F;
    static constexpr float kMinZoom = 0.25F;
    static constexpr float kMaxZoom = 8.0F;

    // Sizes in points; pages whose size is not known yet take the first page's size.
    void reset(std::size_t pageCount, float firstWidthPt, float firstHeightPt);
    void setPageSize(std::size_t page, float widthPt, float heightPt);
    [[nodiscard]] std::size_t pageCount() const noexcept { return m_sizes.size(); }

    // Zoom: points to logical px. Fit width and fit page follow the viewport until the user zooms.
    enum class Fit : std::uint8_t { None, Width, Page };
    void setViewportWidth(float width);
    // True when the zoom changed.
    bool setViewportHeight(float height);
    void setZoom(float zoom);
    void zoomBy(float factor);
    void fitWidth();
    void fitPage();
    [[nodiscard]] float zoom() const noexcept;
    [[nodiscard]] Fit fit() const noexcept { return m_fit; }

    // Page boxes in logical px, stacked top to bottom with kPageGap between them.
    [[nodiscard]] RectF pageBox(std::size_t page) const;
    [[nodiscard]] float contentHeight() const;
    [[nodiscard]] float contentWidth() const;
    // The page under content y (the last page below the end).
    [[nodiscard]] std::size_t pageAt(float y) const;
    // Pages that intersect [top, bottom].
    [[nodiscard]] std::pair<std::size_t, std::size_t> visiblePages(float top, float bottom) const;

    // Search: hits arrive per page in points; the current hit moves in page order and wraps.
    void startSearch(std::string query);
    void clearSearch();
    [[nodiscard]] const std::string& query() const noexcept { return m_query; }
    void setHits(std::size_t page, std::vector<RectF> hitsPt);
    [[nodiscard]] bool searchDone() const noexcept { return m_scanned == m_sizes.size(); }
    // The next page to scan, if any.
    [[nodiscard]] std::optional<std::size_t> nextPageToScan() const;
    [[nodiscard]] std::size_t hitCount() const noexcept { return m_hitCount; }
    // 1-based ordinal of the current hit, 0 when there is none.
    [[nodiscard]] std::size_t currentOrdinal() const;
    [[nodiscard]] const std::vector<RectF>& hits(std::size_t page) const;
    struct HitRef {
      std::size_t page = 0;
      std::size_t index = 0;
    };
    [[nodiscard]] std::optional<HitRef> currentHit() const noexcept { return m_current; }
    void setCurrentHit(HitRef hit) { m_current = hit; }
    // Moves to the next (or previous) hit at or after `fromPage` on the first call; false when there is none.
    bool nextHit(std::size_t fromPage);
    bool previousHit(std::size_t fromPage);

  private:
    [[nodiscard]] float pageWidthPx(std::size_t page) const;
    [[nodiscard]] float pageHeightPx(std::size_t page) const;

    struct Size {
      float width = 0.0F;
      float height = 0.0F;
      bool known = false;
    };
    std::vector<Size> m_sizes;
    float m_defaultWidth = 612.0F;
    float m_defaultHeight = 792.0F;
    float m_viewportWidth = 0.0F;
    float m_viewportHeight = 0.0F;
    float m_zoom = 1.0F;
    Fit m_fit = Fit::Width;

    std::string m_query;
    std::vector<std::vector<RectF>> m_hits;
    std::size_t m_scanned = 0;
    std::size_t m_hitCount = 0;
    std::optional<HitRef> m_current;
  };

} // namespace viewer
