#include "osc1337.h"

#include "osc_frame.h"
#include "render/core/image_decoder.h"
#include "sixel.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>

namespace term {

  namespace {

    constexpr std::uint8_t kEsc = 0x1b;
    constexpr std::uint8_t kBel = 0x07;
    constexpr std::uint8_t kCan = 0x18;
    constexpr std::uint8_t kSub = 0x1a;
    constexpr std::string_view kPrefix = "\x1b]1337;";
    constexpr std::size_t kMaxKey = 16;
    constexpr std::size_t kMaxPayload = 64U << 20; // base64 bytes of one image, all its parts together
    constexpr int kMaxSide = 1 << 14;              // pixels; a larger target is scaled down to it

    bool imageKey(std::string_view key) { return key == "File" || key == "MultipartFile" || key == "FilePart"; }

    // Pixels for one extent; 0 for auto.
    std::optional<int> extentPx(std::string_view value, int cellPx, int totalPx) {
      if (value.empty() || value == "auto") {
        return 0;
      }
      long long unit = cellPx;
      bool percent = false;
      if (value.ends_with("px")) {
        unit = 1;
        value.remove_suffix(2);
      } else if (value.ends_with('%')) {
        percent = true;
        value.remove_suffix(1);
      }
      int n = 0;
      const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
      if (ec != std::errc{} || end != value.data() + value.size() || n <= 0) {
        return std::nullopt;
      }
      const long long px = percent ? static_cast<long long>(totalPx) * n / 100 : unit * n;
      return static_cast<int>(std::clamp(px, 1LL, static_cast<long long>(kMaxSide)));
    }

    // Area average when it shrinks, nearest pixel when it grows; colour weighted by alpha.
    std::vector<std::uint8_t> scale(const std::vector<std::uint8_t>& src, int sw, int sh, int dw, int dh) {
      if (sw == dw && sh == dh) {
        return src;
      }
      std::vector<std::uint8_t> dst(static_cast<std::size_t>(dw) * static_cast<std::size_t>(dh) * 4);
      const auto span = [](int i, int from, int to) {
        const auto lo = static_cast<int>(static_cast<long long>(i) * from / to);
        const auto hi = static_cast<int>((static_cast<long long>(i + 1) * from + to - 1) / to);
        return std::pair{lo, std::max(lo + 1, hi)};
      };
      std::uint8_t* out = dst.data();
      for (int y = 0; y < dh; ++y) {
        const auto [y0, y1] = span(y, sh, dh);
        for (int x = 0; x < dw; ++x, out += 4) {
          const auto [x0, x1] = span(x, sw, dw);
          std::uint64_t a = 0;
          std::uint64_t rgb[3] = {};
          for (int sy = y0; sy < y1; ++sy) {
            const std::uint8_t* p = src.data() + (static_cast<std::size_t>(sy) * static_cast<std::size_t>(sw) + static_cast<std::size_t>(x0)) * 4;
            for (int sx = x0; sx < x1; ++sx, p += 4) {
              a += p[3];
              rgb[0] += static_cast<std::uint64_t>(p[0]) * p[3];
              rgb[1] += static_cast<std::uint64_t>(p[1]) * p[3];
              rgb[2] += static_cast<std::uint64_t>(p[2]) * p[3];
            }
          }
          const auto count = static_cast<std::uint64_t>(y1 - y0) * static_cast<std::uint64_t>(x1 - x0);
          for (int c = 0; c < 3; ++c) {
            out[c] = a == 0 ? 0 : static_cast<std::uint8_t>(rgb[c] / a);
          }
          out[3] = static_cast<std::uint8_t>(a / count);
        }
      }
      return dst;
    }

    void release(std::string& s) { std::string{}.swap(s); }

  } // namespace

  ImageArgs parseImageArgs(std::string_view args) {
    ImageArgs out;
    while (!args.empty()) {
      const std::size_t semi = args.find(';');
      const std::string_view pair = args.substr(0, semi);
      args = semi == std::string_view::npos ? std::string_view{} : args.substr(semi + 1);
      const std::size_t eq = pair.find('=');
      if (eq == std::string_view::npos) {
        continue;
      }
      const std::string_view key = pair.substr(0, eq);
      const std::string_view value = pair.substr(eq + 1);
      if (key == "width") {
        out.width = value;
      } else if (key == "height") {
        out.height = value;
      } else if (key == "preserveAspectRatio") {
        out.preserveAspect = value != "0";
      } else if (key == "inline") {
        out.inlineImage = value == "1";
      }
    }
    return out;
  }

  std::optional<std::pair<int, int>> imageTargetSize(const ImageArgs& args, int imageWidth, int imageHeight,
                                                     const CellGrid& grid) {
    const int cw = std::max(1, grid.cellWidth);
    const int ch = std::max(1, grid.cellHeight);
    const int maxWidth = std::max(1, grid.cols) * cw;
    const auto w = extentPx(args.width, cw, maxWidth);
    const auto h = extentPx(args.height, ch, std::max(1, grid.rows) * ch);
    if (!w || !h || imageWidth <= 0 || imageHeight <= 0) {
      return std::nullopt;
    }
    double tw = *w;
    double th = *h;
    if (tw == 0 && th == 0) {
      tw = imageWidth;
      th = imageHeight;
    } else if (tw == 0) {
      tw = th * imageWidth / imageHeight;
    } else if (th == 0) {
      th = tw * imageHeight / imageWidth;
    } else if (args.preserveAspect) {
      const double s = std::min(tw / imageWidth, th / imageHeight);
      tw = imageWidth * s;
      th = imageHeight * s;
    }
    if (tw > maxWidth) {
      th = th * maxWidth / tw;
      tw = maxWidth;
    }
    if (th > kMaxSide) {
      tw = tw * kMaxSide / th;
      th = kMaxSide;
    }
    return std::pair{std::max(1, static_cast<int>(std::lround(tw))), std::max(1, static_cast<int>(std::lround(th)))};
  }

  void ImageFilter::flush(std::vector<std::uint8_t>& out) {
    out.insert(out.end(), m_held.begin(), m_held.end());
    m_held.clear();
    m_state = State::Ground;
  }

  void ImageFilter::draw(std::string_view args, std::string_view data, std::vector<std::uint8_t>& out, const CellGrid& grid) {
    const ImageArgs parsed = parseImageArgs(args);
    if (!parsed.inlineImage) {
      return;
    }
    const std::string bytes = base64Decode(data);
    // Remote programs send these bytes: only the memory-safe decoders.
    auto image = decodeWuffsImage(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    if (!image) {
      return;
    }
    const auto size = imageTargetSize(parsed, image->width, image->height, grid);
    if (!size) {
      return;
    }
    const auto [w, h] = *size;
    appendKittyImage(scale(image->pixels, image->width, image->height, w, h), w, h, out);
  }

  void ImageFilter::finish(std::vector<std::uint8_t>& out, const CellGrid& grid) {
    const std::string_view key = std::string_view(m_held).substr(kPrefix.size());
    if (key == "File") {
      const std::size_t colon = m_payload.find(':');
      if (colon != std::string::npos) {
        draw(std::string_view(m_payload).substr(0, colon), std::string_view(m_payload).substr(colon + 1), out, grid);
      }
    } else if (key == "MultipartFile") {
      m_multipart = m_payload;
      release(m_parts);
    } else if (key == "FilePart" && m_multipart) {
      if (m_parts.size() + m_payload.size() > kMaxPayload) {
        m_multipart.reset();
        release(m_parts);
      } else {
        m_parts += m_payload;
      }
    } else if (key == "FileEnd" && m_multipart) {
      draw(*m_multipart, m_parts, out, grid);
      m_multipart.reset();
      release(m_parts);
    }
    m_held.clear();
    release(m_payload);
    m_state = State::Ground;
  }

  void ImageFilter::feed(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, const CellGrid& grid) {
    for (std::size_t at = 0; at < in.size(); ++at) {
      const std::uint8_t b = in[at];
      switch (m_state) {
        case State::Ground: {
          if (b == kEsc) {
            m_held.assign(1, static_cast<char>(b));
            m_state = State::Prefix;
            break;
          }
          const auto* esc = static_cast<const std::uint8_t*>(std::memchr(in.data() + at, kEsc, in.size() - at));
          const std::size_t end = esc != nullptr ? static_cast<std::size_t>(esc - in.data()) : in.size();
          out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(at), in.begin() + static_cast<std::ptrdiff_t>(end));
          at = end - 1;
          break;
        }
        case State::Prefix:
          if (static_cast<char>(b) == kPrefix[m_held.size()]) {
            m_held.push_back(static_cast<char>(b));
            if (m_held.size() == kPrefix.size()) {
              m_state = State::Key;
            }
          } else {
            flush(out);
            --at; // b starts over in Ground
          }
          break;
        case State::Key: {
          const std::string_view key = std::string_view(m_held).substr(kPrefix.size());
          if (b == '=' && imageKey(key)) {
            m_payload.clear();
            m_state = State::Payload;
          } else if ((b == kBel || b == kEsc) && key == "FileEnd") {
            if (b == kBel) {
              finish(out, grid);
            } else {
              m_state = State::PayloadEscape;
            }
          } else if (std::isalpha(b) != 0 && key.size() < kMaxKey) {
            m_held.push_back(static_cast<char>(b));
          } else {
            flush(out);
            --at;
          }
          break;
        }
        case State::Payload: {
          const auto* const stop = std::find_if(in.data() + at, in.data() + in.size(), [](std::uint8_t c) {
            return c == kBel || c == kEsc || c == kCan || c == kSub;
          });
          const auto run = static_cast<std::size_t>(stop - (in.data() + at));
          if (m_payload.size() + run > kMaxPayload) {
            m_multipart.reset();
            release(m_parts);
            release(m_payload);
            m_held.clear();
            m_state = State::Skip;
            --at;
            break;
          }
          m_payload.append(reinterpret_cast<const char*>(in.data() + at), run);
          at += run;
          if (at == in.size()) {
            break;
          }
          if (in[at] == kBel) {
            finish(out, grid);
          } else if (in[at] == kEsc) {
            m_state = State::PayloadEscape;
          } else { // CAN or SUB cancels the sequence
            m_held.clear();
            release(m_payload);
            m_state = State::Ground;
            out.push_back(in[at]);
          }
          break;
        }
        case State::PayloadEscape:
          if (b == '\\') {
            finish(out, grid);
          } else { // ESC ends the OSC without ST: drop it; ESC starts a new sequence
            release(m_payload);
            m_held.assign(1, static_cast<char>(kEsc));
            m_state = State::Prefix;
            --at;
          }
          break;
        case State::Skip:
          if (b == kBel) {
            m_state = State::Ground;
          } else if (b == kEsc) {
            m_state = State::SkipEscape;
          } else if (b == kCan || b == kSub) {
            m_state = State::Ground;
            out.push_back(b);
          }
          break;
        case State::SkipEscape:
          if (b == '\\') {
            m_state = State::Ground;
          } else {
            m_held.assign(1, static_cast<char>(kEsc));
            m_state = State::Prefix;
            --at;
          }
          break;
      }
    }
  }

} // namespace term
