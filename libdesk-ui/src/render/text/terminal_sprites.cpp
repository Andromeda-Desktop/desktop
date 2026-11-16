#include "render/text/terminal_sprites.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace terminal_sprites {

  namespace {

    // Arms of U+2500-257F as "up right down left": 0 none, 1 light, 2 heavy, 3 double; "" is drawn elsewhere.
    constexpr std::array<std::string_view, 128> kBox = {
        "0101", "0202", "1010", "2020", "",     "",     "",     "",     "",     "",     "",     "",     "0110", "0210",
        "0120", "0220", "0011", "0012", "0021", "0022", "1100", "1200", "2100", "2200", "1001", "1002", "2001", "2002",
        "1110", "1210", "2110", "1120", "2120", "2210", "1220", "2220", "1011", "1012", "2011", "1021", "2021", "2012",
        "1022", "2022", "0111", "0112", "0211", "0212", "0121", "0122", "0221", "0222", "1101", "1102", "1201", "1202",
        "2101", "2102", "2201", "2202", "1111", "1112", "1211", "1212", "2111", "1121", "2121", "2112", "2211", "1122",
        "1221", "2212", "1222", "2122", "2221", "2222", "",     "",     "",     "",     "0303", "3030", "0310", "0130",
        "0330", "0013", "0031", "0033", "1300", "3100", "3300", "1003", "3001", "3003", "1310", "3130", "3330", "1013",
        "3031", "3033", "0313", "0131", "0333", "1303", "3101", "3303", "1313", "3131", "3333", "",     "",     "",
        "",     "",     "",     "",     "0001", "1000", "0100", "0010", "0002", "2000", "0200", "0020", "0201", "1020",
        "0102", "2010"};

    class Canvas {
    public:
      Canvas(int w, int h, std::vector<std::uint8_t>& a) : m_w(w), m_h(h), m_a(a) { m_a.assign(std::size_t(w) * h, 0); }

      void fill(int x0, int y0, int x1, int y1, std::uint8_t value = 255) {
        x0 = std::clamp(x0, 0, m_w);
        x1 = std::clamp(x1, 0, m_w);
        y0 = std::clamp(y0, 0, m_h);
        y1 = std::clamp(y1, 0, m_h);
        for (int y = y0; y < y1; ++y) {
          std::fill_n(m_a.begin() + std::ptrdiff_t(y) * m_w + x0, std::max(0, x1 - x0), value);
        }
      }

      // Anti-aliased by 4x4 supersampling; inside(x, y) takes pixel coordinates.
      template <typename F> void shape(F inside) {
        for (int y = 0; y < m_h; ++y) {
          for (int x = 0; x < m_w; ++x) {
            int n = 0;
            for (int sy = 0; sy < 4; ++sy) {
              for (int sx = 0; sx < 4; ++sx) {
                n += inside(float(x) + (float(sx) + 0.5F) / 4.0F, float(y) + (float(sy) + 0.5F) / 4.0F) ? 1 : 0;
              }
            }
            auto& px = m_a[std::size_t(y) * m_w + x];
            px = std::max<std::uint8_t>(px, std::uint8_t(n * 255 / 16));
          }
        }
      }

      [[nodiscard]] int w() const { return m_w; }
      [[nodiscard]] int h() const { return m_h; }

    private:
      int m_w;
      int m_h;
      std::vector<std::uint8_t>& m_a;
    };

    float segmentDistance(float px, float py, float ax, float ay, float bx, float by) {
      const float dx = bx - ax;
      const float dy = by - ay;
      const float t = std::clamp(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy), 0.0F, 1.0F);
      return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
    }

    // Start of a band of the given thickness centred in size.
    int lo(int size, int thickness) { return (size - thickness) / 2; }
    int hi(int size, int thickness) { return lo(size, thickness) + thickness; }

    void box(Canvas& c, std::string_view arms, int light) {
      const int heavy = light * 2;
      const auto thick = [&](char weight) { return weight == '1' ? light : weight == '2' ? heavy : weight == '3' ? 3 * light : 0; };
      const int w = c.w();
      const int h = c.h();
      const char up = arms[0], right = arms[1], down = arms[2], left = arms[3];
      const int vertical = std::max(thick(up), thick(down));
      const int horizontal = std::max(thick(left), thick(right));
      // An arm reaches over the full width of the perpendicular stroke so joins are square.
      const auto armH = [&](char weight, bool toLeft) {
        if (weight == '0') return;
        const int t = thick(weight);
        const int span = vertical > 0 ? vertical : t;
        toLeft ? c.fill(0, lo(h, t), hi(w, span), hi(h, t)) : c.fill(lo(w, span), lo(h, t), w, hi(h, t));
      };
      const auto armV = [&](char weight, bool toUp) {
        if (weight == '0') return;
        const int t = thick(weight);
        const int span = horizontal > 0 ? horizontal : t;
        toUp ? c.fill(lo(w, t), 0, hi(w, t), hi(h, span)) : c.fill(lo(w, t), lo(h, span), hi(w, t), h);
      };
      armH(left, true);
      armH(right, false);
      armV(up, true);
      armV(down, false);
      // A double arm is a 3-light band with its middle light line cut out. The cut stops at a single perpendicular
      // stroke (which then crosses it) and runs into a double one's cut (which makes the inner corner).
      const bool doubleV = up == '3' || down == '3';
      const bool doubleH = left == '3' || right == '3';
      if (left == '3') c.fill(0, lo(h, light), doubleV ? hi(w, light) : vertical > 0 ? lo(w, vertical) : w, hi(h, light), 0);
      if (right == '3') c.fill(doubleV ? lo(w, light) : vertical > 0 ? hi(w, vertical) : 0, lo(h, light), w, hi(h, light), 0);
      if (up == '3') c.fill(lo(w, light), 0, hi(w, light), doubleH ? hi(h, light) : horizontal > 0 ? lo(h, horizontal) : h, 0);
      if (down == '3') c.fill(lo(w, light), doubleH ? lo(h, light) : horizontal > 0 ? hi(h, horizontal) : 0, hi(w, light), h, 0);
    }

    void dashes(Canvas& c, bool vertical, int thickness, int count) {
      const int length = vertical ? c.h() : c.w();
      const int gap = std::max(1, length / (count * 4));
      for (int i = 0; i < count; ++i) {
        const int a = i * length / count + gap / 2;
        const int b = (i + 1) * length / count - (gap - gap / 2);
        vertical ? c.fill(lo(c.w(), thickness), a, hi(c.w(), thickness), b)
                 : c.fill(a, lo(c.h(), thickness), b, hi(c.h(), thickness));
      }
    }

    // A rounded corner joining the centre lines of two edges: sx, sy pick the edges (+1 right / down).
    void arc(Canvas& c, int light, int sx, int sy) {
      const float cx = float(lo(c.w(), light)) + float(light) / 2.0F;
      const float cy = float(lo(c.h(), light)) + float(light) / 2.0F;
      const float r = std::min(float(c.w()) / 2.0F, float(c.h()) / 2.0F);
      const float ox = cx + float(sx) * r;
      const float oy = cy + float(sy) * r;
      const float half = float(light) / 2.0F;
      c.shape([&](float x, float y) {
        const bool inQuadrant = (x - ox) * float(sx) <= 0 && (y - oy) * float(sy) <= 0;
        if (inQuadrant) return std::abs(std::hypot(x - ox, y - oy) - r) <= half;
        // Straight runs from the arc to the cell edges.
        const bool vertical = std::abs(x - cx) <= half && (y - oy) * float(sy) > 0;
        const bool horizontal = std::abs(y - cy) <= half && (x - ox) * float(sx) > 0;
        return vertical || horizontal;
      });
    }

    void diagonal(Canvas& c, int light, bool rising, bool falling) {
      const float w = float(c.w());
      const float h = float(c.h());
      const float half = float(light) / 2.0F;
      c.shape([&](float x, float y) {
        return (rising && segmentDistance(x, y, w, 0, 0, h) <= half) || (falling && segmentDistance(x, y, 0, 0, w, h) <= half);
      });
    }

    void block(Canvas& c, char32_t cp) {
      const int w = c.w();
      const int h = c.h();
      const auto eighthY = [&](int k) { return int(std::lround(double(h) * k / 8)); };
      const auto eighthX = [&](int k) { return int(std::lround(double(w) * k / 8)); };
      if (cp == 0x2580) c.fill(0, 0, w, h / 2);
      else if (cp >= 0x2581 && cp <= 0x2588) c.fill(0, h - eighthY(int(cp - 0x2580)), w, h);
      else if (cp >= 0x2589 && cp <= 0x258F) c.fill(0, 0, eighthX(int(0x2590 - cp)), h);
      else if (cp == 0x2590) c.fill(w / 2, 0, w, h);
      else if (cp >= 0x2591 && cp <= 0x2593) c.fill(0, 0, w, h, std::uint8_t(64 * (cp - 0x2590)));
      else if (cp == 0x2594) c.fill(0, 0, w, eighthY(1));
      else if (cp == 0x2595) c.fill(w - eighthX(1), 0, w, h);
      else {
        // Quadrants as bits: 1 upper left, 2 upper right, 4 lower left, 8 lower right.
        constexpr std::array<int, 10> kQuadrants = {4, 8, 1, 1 | 4 | 8, 1 | 8, 1 | 2 | 4, 1 | 2 | 8, 2, 2 | 4, 2 | 4 | 8};
        const int q = kQuadrants[cp - 0x2596];
        if (q & 1) c.fill(0, 0, w / 2, h / 2);
        if (q & 2) c.fill(w / 2, 0, w, h / 2);
        if (q & 4) c.fill(0, h / 2, w / 2, h);
        if (q & 8) c.fill(w / 2, h / 2, w, h);
      }
    }

    void braille(Canvas& c, unsigned dots) {
      // Dot bits 0-7 are dots 1-8: column 0 rows 0-2, column 1 rows 0-2, then row 3 of columns 0 and 1.
      constexpr std::array<std::pair<int, int>, 8> kDot = {{{0, 0}, {0, 1}, {0, 2}, {1, 0}, {1, 1}, {1, 2}, {0, 3}, {1, 3}}};
      const float w = float(c.w());
      const float h = float(c.h());
      const float r = std::max(0.75F, std::min(w / 4.0F, h / 8.0F) * 0.7F);
      c.shape([&](float x, float y) {
        for (int i = 0; i < 8; ++i) {
          if ((dots & (1U << i)) == 0) continue;
          const float dx = x - w * float(1 + 2 * kDot[i].first) / 4.0F;
          const float dy = y - h * float(1 + 2 * kDot[i].second) / 8.0F;
          if (dx * dx + dy * dy <= r * r) return true;
        }
        return false;
      });
    }

    void powerline(Canvas& c, char32_t cp, int light) {
      const float w = float(c.w());
      const float h = float(c.h());
      const float half = float(light) / 2.0F;
      const bool mirror = (cp - 0xE0B0) % 4 >= 2; // E0B2/3, E0B6/7: point left
      const auto mx = [&](float x) { return mirror ? w - x : x; };
      switch (cp) {
        case 0xE0B0:
        case 0xE0B2: c.shape([&](float x, float y) { return mx(x) <= w * (1.0F - std::abs(2.0F * y / h - 1.0F)); }); break;
        case 0xE0B1:
        case 0xE0B3:
          c.shape([&](float x, float y) {
            const float px = mx(x);
            return segmentDistance(px, y, 0, 0, w - half, h / 2) <= half || segmentDistance(px, y, w - half, h / 2, 0, h) <= half;
          });
          break;
        case 0xE0B4:
        case 0xE0B6:
          c.shape([&](float x, float y) {
            const float dy = (y - h / 2) / (h / 2);
            return mx(x) / w <= std::sqrt(std::max(0.0F, 1.0F - dy * dy));
          });
          break;
        case 0xE0B5:
        case 0xE0B7:
          c.shape([&](float x, float y) {
            const float px = mx(x);
            const float dy = (y - h / 2) / (h / 2);
            const float edge = (w - half) * std::sqrt(std::max(0.0F, 1.0F - dy * dy));
            return std::abs(px - edge) <= half || (px <= half && std::abs(dy) >= 0.98F);
          });
          break;
        case 0xE0B8: c.shape([&](float x, float y) { return x / w <= y / h; }); break;           // lower left
        case 0xE0BA: c.shape([&](float x, float y) { return x / w >= 1.0F - y / h; }); break;    // lower right
        case 0xE0BC: c.shape([&](float x, float y) { return x / w <= 1.0F - y / h; }); break;    // upper left
        case 0xE0BE: c.shape([&](float x, float y) { return x / w >= y / h; }); break;           // upper right
        case 0xE0B9:
        case 0xE0BF: diagonal(c, light, false, true); break;
        case 0xE0BB:
        case 0xE0BD: diagonal(c, light, true, false); break;
        default: break;
      }
    }

  } // namespace

  bool covers(char32_t cp) noexcept {
    return (cp >= 0x2500 && cp <= 0x259F) || (cp >= 0x2800 && cp <= 0x28FF) || (cp >= 0xE0B0 && cp <= 0xE0BF);
  }

  bool draw(char32_t cp, int width, int height, int stroke, std::vector<std::uint8_t>& alpha) {
    if (!covers(cp) || width <= 0 || height <= 0) {
      return false;
    }
    Canvas c(width, height, alpha);
    const int light = std::max(1, stroke);
    if (cp >= 0x2800) {
      cp >= 0xE0B0 ? powerline(c, cp, light) : braille(c, unsigned(cp - 0x2800));
      return true;
    }
    if (cp >= 0x2580) {
      block(c, cp);
      return true;
    }
    if (const std::string_view arms = kBox[cp - 0x2500]; !arms.empty()) {
      box(c, arms, light);
      return true;
    }
    switch (cp) {
      case 0x2504: dashes(c, false, light, 3); break;
      case 0x2505: dashes(c, false, 2 * light, 3); break;
      case 0x2506: dashes(c, true, light, 3); break;
      case 0x2507: dashes(c, true, 2 * light, 3); break;
      case 0x2508: dashes(c, false, light, 4); break;
      case 0x2509: dashes(c, false, 2 * light, 4); break;
      case 0x250A: dashes(c, true, light, 4); break;
      case 0x250B: dashes(c, true, 2 * light, 4); break;
      case 0x254C: dashes(c, false, light, 2); break;
      case 0x254D: dashes(c, false, 2 * light, 2); break;
      case 0x254E: dashes(c, true, light, 2); break;
      case 0x254F: dashes(c, true, 2 * light, 2); break;
      case 0x256D: arc(c, light, 1, 1); break;
      case 0x256E: arc(c, light, -1, 1); break;
      case 0x256F: arc(c, light, -1, -1); break;
      case 0x2570: arc(c, light, 1, -1); break;
      case 0x2571: diagonal(c, light, true, false); break;
      case 0x2572: diagonal(c, light, false, true); break;
      case 0x2573: diagonal(c, light, true, true); break;
      default: return false;
    }
    return true;
  }

} // namespace terminal_sprites
