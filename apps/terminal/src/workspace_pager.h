#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <variant>

namespace term {

  // The sidebar pager of workspaces (rulebook §7.7): a two-finger swipe tracks 1:1, rubber-bands past the ends, and
  // on release projects its end point and settles on one page. Positions are px from page 0 (page i at i × width).
  class WorkspacePager {
  public:
    struct Rest {
      std::size_t page;
    };
    struct Tracking {
      std::size_t from; // the page shown when the finger came down
    };
    struct Settling {
      std::size_t target;
    };
    using Mode = std::variant<Rest, Tracking, Settling>;

    // A change of the workspace list or of the shown workspace: a finger or a slide in progress ends on `current`.
    void setPages(std::size_t count, std::size_t current);
    void setWidth(float width);

    // dx: the finger's motion in px toward later pages; time in ms. Rest and Settling become Tracking.
    void track(float dx, std::uint32_t timeMs);
    // Tracking -> Settling; returns the target. Other modes: no change, returns page().
    std::size_t release();
    // A click or a key: slides to `page` (Rest/Settling); ignored while the finger is down.
    void goTo(std::size_t page);
    // The settle animation's frame (Settling only).
    void animate(float position);
    // The settle animation ended: Settling -> Rest at its target.
    void settled();

    [[nodiscard]] const Mode& mode() const noexcept { return m_mode; }
    [[nodiscard]] std::size_t count() const noexcept { return m_count; }
    // Rest: its page; Tracking: the page the finger started on; Settling: the target.
    [[nodiscard]] std::size_t page() const;
    // The finger's or the animation's position, without the rubber band.
    [[nodiscard]] float position() const noexcept { return m_position; }
    // What to draw: the position with the rubber band past either end.
    [[nodiscard]] float shown() const;
    // px/s from the last 100 ms of finger samples (least squares).
    [[nodiscard]] float velocity() const;
    [[nodiscard]] float width() const noexcept { return m_width; }

  private:
    struct Sample {
      std::uint32_t time;
      float position;
    };
    Mode m_mode = Rest{0};
    std::size_t m_count = 1;
    float m_width = 0.0F;
    float m_position = 0.0F;
    std::deque<Sample> m_samples;
    [[nodiscard]] float maxPosition() const { return static_cast<float>(m_count - 1) * m_width; }
  };

} // namespace term
