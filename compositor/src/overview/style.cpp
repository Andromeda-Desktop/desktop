#include "overview/style.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace andromeda::ov {

  namespace {

    constexpr std::array<std::string_view, 3> kStyleNames = {"spaces", "taskview", "strip"};

    // Spaces + Task View (overview handoff): one row of workspace pieces, their height a share of the output's,
    // sized down when they would not fit between the frame margins.
    constexpr double kThumbGap = 16;
    constexpr double kSpacesThumbShare = 0.11;
    constexpr double kTaskViewThumbShare = 0.12;
    constexpr double kThumbMinHeight = 56;
    constexpr double kThumbMaxHeight = 128;
    constexpr double kPlusShare = 0.5;   // the + piece is half a piece wide
    constexpr double kFrameMargin = 40;  // the current workspace's frame to the output's side edges
    constexpr double kThumbTop = 52;     // Spaces: piece row from the top edge, below the center island
    constexpr double kSpacesFrameGap = 76; // Spaces: piece row to the frame (names, then the filter field)
    constexpr double kSpacesFilterGap = 28; // Spaces: piece row to the filter field (the names sit between)
    constexpr double kSpacesBottom = 32; // Spaces: frame to the bottom edge
    constexpr double kThumbBottom = 28;  // Task View: piece row above the bottom edge
    constexpr double kTaskViewTop = 104; // Task View: frame top when the filter field sits at kTopFilterY
    constexpr double kMainBottom = 48;   // Task View: frame to the piece row (names sit above the pieces)
    constexpr double kTopFilterY = 52;   // Task View and Strip: filter field top, at least
    constexpr double kBarFilterGap = 20; // Task View and Strip: work-area top (the bar) to the filter field
    constexpr double kBarThumbGap = 8;   // Spaces: work-area top (the bar) to the piece row (prototype: 44 -> 52)

    // Task View and Strip: the field sits under the bar however tall ui_scale makes it.
    double topFilterY(const Input& in) { return std::max(kTopFilterY * in.ui, in.usable.y + kBarFilterGap * in.ui); }
    constexpr double kExposeInsetX = 24; // Spaces: exposé inside the frame
    constexpr double kExposeInsetTop = 20;
    constexpr double kExposeInsetBottom = 12;
    constexpr double kMinimizedOpacity = 0.55;

    constexpr double kStripGap = 64;      // between Strip rows: room for the next row's name above it
    constexpr double kStripShift = 14;    // Strip: the current row sits this far below the centre, under the filter
    constexpr double kStripInset = 8;     // Strip: a window keeps this far inside its row's right and bottom edges
    constexpr double kPeekOpacity = 0.6;     // Strip: a neighbouring workspace row one step away (mockup B)
    constexpr double kStripPlusShare = 0.45; // the Strip's New row is this share of a row's extent along the axis
    constexpr double kDimmed = 0.5;
    constexpr double kHiddenFade = 2; // a window the placement hides is gone halfway into the open


    WindowPlace win(uint32_t id, size_t workspace, const Rect& rect, double opacity, bool dimmed = false) {
      WindowPlace w;
      w.id = id;
      w.workspace = workspace;
      w.rect = rect;
      w.opacity = opacity;
      w.dimmed = dimmed;
      return w;
    }

    Fit thumbFit(const Rect& card, double outputWidth) { return {card.w / outputWidth, card.x, card.y}; }

    // The part of `desk` outside the output, so a window shrunk into a workspace miniature stays inside it.
    std::optional<Insets> outputClip(const Rect& r, const Input& in) {
      const Insets cut{
          std::max(0.0, -r.y), std::max(0.0, r.x + r.w - in.width), std::max(0.0, r.y + r.h - in.height),
          std::max(0.0, -r.x)
      };
      return cut == Insets{} ? std::nullopt : std::optional<Insets>{cut};
    }

    // A member wholly off the output has nothing left after outputClip, so it is not drawn.
    double memberOpacity(const Member& m, const Input& in) {
      const bool onOutput =
          m.desk.x < in.width && m.desk.x + m.desk.w > 0 && m.desk.y < in.height && m.desk.y + m.desk.h > 0;
      return m.minimized || !onOutput ? 0.0 : 1.0;
    }

    // Members of each workspace shrink into its thumb, cut to the output, so the thumb is a live miniature.
    void placeIntoCards(Placement& out, const Input& in) {
      for (const Card& card : out.cards) {
        const bool copy = card.workspace == in.current && card.kind == CardKind::Thumb;
        const Fit fit = thumbFit(card.rect, in.width);
        for (const Member& m : in.workspaces[card.workspace].members) {
          WindowPlace w = win(m.id, card.workspace, mapRect(m.desk, fit), memberOpacity(m, in));
          w.clip = outputClip(m.desk, in);
          (copy ? out.currentThumb : out.windows).push_back(w);
        }
      }
    }

    Placement placeBar(const Input& in, bool top) {
      Placement out;
      const size_t n = in.workspaces.size();
      const double ar = in.width / std::max(in.height, 1.0);
      double th = std::clamp(in.height * (top ? kSpacesThumbShare : kTaskViewThumbShare), kThumbMinHeight, kThumbMaxHeight);
      double tw = th * ar;
      const double avail = in.width - 2 * kFrameMargin;
      const auto need = [&] { return static_cast<double>(n) * tw + tw * kPlusShare + static_cast<double>(n) * kThumbGap; };
      if (need() > avail) {
        tw = std::max(1.0, (avail - static_cast<double>(n) * kThumbGap) / (static_cast<double>(n) + kPlusShare));
        th = tw / ar;
      }
      const double y = top ? std::max(kThumbTop * in.ui, in.usable.y + kBarThumbGap * in.ui) : in.height - kThumbBottom * in.ui - th;
      double x = (in.width - need()) / 2;
      for (size_t i = 0; i < n; ++i) {
        out.cards.push_back({i, {x, y, tw, th}, CardKind::Thumb});
        x += tw + kThumbGap;
      }
      out.plus = Rect{x, y, tw * kPlusShare, th};

      const double frameTop = top ? y + th + kSpacesFrameGap * in.ui : topFilterY(in) + (kTaskViewTop - kTopFilterY) * in.ui;
      const double frameBottom = top ? in.height - kSpacesBottom : y - kMainBottom * in.ui;
      const Fit frameFit = fitIn(
          {0, 0, in.width, in.height},
          {kFrameMargin, frameTop, in.width - 2 * kFrameMargin, std::max(1.0, frameBottom - frameTop)}
      );
      const Rect main = mapRect({0, 0, in.width, in.height}, frameFit);
      out.main = main;
      out.mainFit = frameFit;
      const double filterY = top ? y + th + kSpacesFilterGap * in.ui : topFilterY(in);
      out.filter = Point{(in.width - kFilterWidth * in.ui) / 2, filterY};

      if (in.current < n) {
        const Workspace& ws = in.workspaces[in.current];
        if (!top || ws.arrangement == Arrangement::Free) {
          std::vector<ExposeItem> items;
          items.reserve(ws.members.size());
          for (const Member& m : ws.members) {
            items.push_back({m.id, m.desk});
          }
          const Rect area = top ? Rect{
              main.x + kExposeInsetX, main.y + kExposeInsetTop, main.w - 2 * kExposeInsetX,
              main.h - kExposeInsetTop - kExposeInsetBottom
          } : main;
          const std::vector<ExposeItem> spread = exposeGrid(items, area);
          for (size_t i = 0; i < spread.size(); ++i) {
            out.windows.push_back(
                win(spread[i].id, in.current, spread[i].rect, ws.members[i].minimized ? kMinimizedOpacity : 1.0)
            );
          }
        } else {
          const Rect& u = in.usable;
          Rect content = u;
          // Scrolling frames the whole screen, not the usable area: a fullscreen or edge-maximized window reaches under
          // the bar and the dock.
          if (ws.arrangement == Arrangement::Scrolling) {
            double x0 = 0;
            double x1 = in.width;
            for (const Member& m : ws.members) {
              if (!m.minimized) {
                x0 = std::min(x0, m.desk.x);
                x1 = std::max(x1, m.desk.x + m.desk.w);
              }
            }
            content = {x0 - 8, -8, x1 - x0 + 16, in.height + 16};
          }
          const Fit fit = fitIn(content, main);
          out.mainFit = fit;
          if (ws.arrangement == Arrangement::Scrolling) {
            out.viewport = mapRect({-4, -4, in.width + 8, in.height + 8}, fit);
          }
          const Fit thumb = thumbFit(out.cards[in.current].rect, in.width);
          for (const Member& m : ws.members) {
            if (m.minimized) {
              out.windows.push_back(win(m.id, in.current, mapRect(m.desk, thumb), 0.0));
              continue;
            }
            const bool away = ws.arrangement == Arrangement::Scrolling && offscreen(m.desk, in.width);
            out.windows.push_back(win(m.id, in.current, mapRect(m.desk, fit), 1.0, away));
          }
        }
      }
      placeIntoCards(out, in);
      return out;
    }

    // Rows are whole pixels, so a column scrolled by whole rows puts every row on the pixel grid.
    Placement placeStrip(const Input& in) {
      Placement out;
      const size_t n = in.workspaces.size();
      const double s = std::clamp(in.stripScale, 0.05, 1.0);
      const double fw = std::max(1.0, std::round(in.width * s));
      const double fh = std::max(1.0, std::round(in.height * s));
      const double fx = std::round((in.width - fw) / 2);
      const double fy = std::round((in.height - fh) / 2 + kStripShift);
      const double step = (in.horizontal ? fw : fh) + kStripGap;
      const double row = in.stripRow.value_or(static_cast<double>(in.current));
      const auto frameAt = [&](double i) {
        const double offset = (i - row) * step;
        return in.horizontal ? Rect{fx + offset, fy, fw, fh} : Rect{fx, fy + offset, fw, fh};
      };
      for (size_t i = 0; i < n; ++i) {
        const Rect frame = frameAt(static_cast<double>(i));
        out.cards.push_back({i, frame, CardKind::Frame});
        const Fit fit{s, frame.x, frame.y};
        // Full on the chosen row, kPeekOpacity one row away, 0 two rows away: a row fades in and out as it scrolls.
        const double away = std::abs(static_cast<double>(i) - row);
        const double peek = away <= 1.0 ? 1.0 - (1.0 - kPeekOpacity) * away : kPeekOpacity * std::max(0.0, 2.0 - away);
        for (const Member& m : in.workspaces[i].members) {
          const bool away = in.workspaces[i].arrangement == Arrangement::Scrolling && offscreen(m.desk, in.width);
          const Rect r = mapRect(m.desk, fit);
          // A window past the output edge is cut at full size where the frame ends, as the screen edge cuts it; it is
          // never shrunk. Only those sides are cut, so a window on screen keeps its rounded corners.
          const Insets cut{
              m.desk.y < 0 ? std::max(0.0, frame.y - r.y) / s : 0.0,
              m.desk.x + m.desk.w > in.width ? std::max(0.0, r.x + r.w - (frame.x + frame.w - kStripInset)) / s : 0.0,
              m.desk.y + m.desk.h > in.height ? std::max(0.0, r.y + r.h - (frame.y + frame.h - kStripInset)) / s : 0.0,
              m.desk.x < 0 ? std::max(0.0, frame.x - r.x) / s : 0.0
          };
          const bool inside = cut.left + cut.right < m.desk.w && cut.top + cut.bottom < m.desk.h;
          const bool drawn = inside && away < 2.0; // ±1 rows at rest (FRAME.md cost)
          WindowPlace w = win(m.id, i, r, m.minimized || !drawn ? 0.0 : peek, away);
          if (cut != Insets{}) {
            w.clip = cut;
          }
          out.windows.push_back(w);
        }
      }
      Rect plus = frameAt(static_cast<double>(n));
      (in.horizontal ? plus.w : plus.h) *= kStripPlusShare;
      out.plus = plus;
      out.filter = Point{(in.width - kFilterWidth * in.ui) / 2, topFilterY(in)};
      return out;
    }

    double lerp(double a, double b, double t) { return a + (b - a) * t; }

    Insets lerpInsets(const Insets& a, const Insets& b, double t) {
      return {lerp(a.top, b.top, t), lerp(a.right, b.right, t), lerp(a.bottom, b.bottom, t), lerp(a.left, b.left, t)};
    }

    // Spaces and Task View between two chosen workspaces: every window, card, area and chrome anchor moves from its
    // place in `a` to its place in `b`.
    Placement blend(const Placement& a, const Placement& b, double t) {
      Placement out = t < 0.5 ? a : b;
      out.windows.clear();
      for (const WindowPlace& from : a.windows) {
        const WindowPlace* to = findWindow(b, from.id);
        if (to == nullptr) {
          out.windows.push_back(from);
          continue;
        }
        WindowPlace w = t < 0.5 ? from : *to;
        w.rect = lerpRect(from.rect, to->rect, t);
        w.opacity = lerp(from.opacity, to->opacity, t);
        if (from.clip || to->clip) {
          w.clip = lerpInsets(from.clip.value_or(Insets{}), to->clip.value_or(Insets{}), t);
        }
        out.windows.push_back(w);
      }
      for (size_t i = 0; i < out.cards.size() && i < a.cards.size() && i < b.cards.size(); ++i) {
        out.cards[i].rect = lerpRect(a.cards[i].rect, b.cards[i].rect, t);
      }
      if (a.viewport && b.viewport) {
        out.viewport = lerpRect(*a.viewport, *b.viewport, t);
      }
      if (a.main && b.main) {
        out.main = lerpRect(*a.main, *b.main, t);
      }
      if (a.mainFit && b.mainFit) {
        out.mainFit = Fit{lerp(a.mainFit->s, b.mainFit->s, t), lerp(a.mainFit->ox, b.mainFit->ox, t),
                          lerp(a.mainFit->oy, b.mainFit->oy, t)};
      }
      if (a.plus && b.plus) {
        out.plus = lerpRect(*a.plus, *b.plus, t);
      }
      if (a.filter && b.filter) {
        out.filter = Point{lerp(a.filter->x, b.filter->x, t), lerp(a.filter->y, b.filter->y, t)};
      }
      return out;
    }

  } // namespace

  std::string_view styleName(Style style) { return kStyleNames[static_cast<size_t>(style)]; }

  std::optional<Style> styleNamed(std::string_view name) {
    for (size_t i = 0; i < kStyleNames.size(); ++i) {
      if (kStyleNames[i] == name) {
        return static_cast<Style>(i);
      }
    }
    return std::nullopt;
  }

  Placement place(Style style, const Input& input) {
    if (input.width <= 0 || input.height <= 0) {
      return {};
    }
    switch (style) {
    case Style::Spaces:
      return placeBar(input, true);
    case Style::TaskView:
      return placeBar(input, false);
    case Style::Strip:
      return placeStrip(input);
    }
    return {};
  }

  const WindowPlace* findWindow(const Placement& placement, uint32_t id) {
    const auto it = std::ranges::find(placement.windows, id, &WindowPlace::id);
    return it == placement.windows.end() ? nullptr : &*it;
  }

  Placement placeAt(Style style, Input input, double row) {
    const size_t n = input.workspaces.size();
    if (n == 0) {
      return place(style, input);
    }
    row = std::clamp(row, 0.0, static_cast<double>(n - 1));
    input.current = static_cast<size_t>(std::lround(row));
    input.stripRow = row;
    if (style != Style::Spaces && style != Style::TaskView) {
      return place(style, input);
    }
    const double lower = std::floor(row);
    const double t = row - lower;
    input.current = static_cast<size_t>(lower);
    const Placement a = place(style, input);
    if (t < 1e-6 || input.current + 1 >= n) {
      return a;
    }
    input.current += 1;
    return blend(a, place(style, input), t);
  }

  Rect deskRect(const Input& input, const Rect& desk, size_t workspace, double row) {
    const double offset = (static_cast<double>(workspace) - row) * (input.horizontal ? input.width : input.height);
    return input.horizontal ? Rect{desk.x + offset, desk.y, desk.w, desk.h}
                            : Rect{desk.x, desk.y + offset, desk.w, desk.h};
  }

  Rect cardDeskRect(const Input& input, const Card& card, double row) {
    // A thumb does not travel: it fades in its slot.
    if (card.kind == CardKind::Thumb) {
      return card.rect;
    }
    return deskRect(input, {0, 0, input.width, input.height}, card.workspace, row);
  }

  double windowOpacity(const WindowPlace& place, bool minimized, double p) {
    p = std::clamp(p, 0.0, 1.0);
    double opacity = 1;
    if (minimized) {
      opacity = place.opacity * p;
    } else if (place.opacity <= 0) {
      opacity = 1 - std::min(1.0, p * kHiddenFade);
    } else {
      opacity = lerp(1, place.opacity, p);
    }
    return place.dimmed ? opacity * lerp(1, kDimmed, p) : opacity;
  }

} // namespace andromeda::ov
