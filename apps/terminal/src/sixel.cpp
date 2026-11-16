#include "sixel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

namespace term {

  namespace {

    constexpr std::uint8_t kEsc = 0x1b;
    constexpr std::uint8_t kCan = 0x18;
    constexpr std::uint8_t kSub = 0x1a;
    constexpr std::size_t kMaxParams = 32; // bytes of DCS parameters before the sequence is not sixel
    constexpr int kMaxParam = 1 << 20;
    constexpr std::size_t kChunk = 4096; // base64 bytes per kitty graphics chunk

    std::uint32_t pack(int r, int g, int b) {
      return static_cast<std::uint32_t>(r) | static_cast<std::uint32_t>(g) << 8 | static_cast<std::uint32_t>(b) << 16
             | 0xFF000000U;
    }

    // Sixel colour components are percentages.
    std::uint32_t percent(int r, int g, int b) {
      const auto c = [](int v) { return std::clamp(v, 0, 100) * 255 / 100; };
      return pack(c(r), c(g), c(b));
    }

    // DEC HLS puts blue at 0 degrees and red at 120: standard hue is 240 degrees on.
    std::uint32_t hls(int hue, int lightness, int saturation) {
      const float h = static_cast<float>((std::clamp(hue, 0, 360) + 240) % 360) / 60.0F;
      const float l = static_cast<float>(std::clamp(lightness, 0, 100)) / 100.0F;
      const float s = static_cast<float>(std::clamp(saturation, 0, 100)) / 100.0F;
      const float chroma = (1.0F - std::fabs(2.0F * l - 1.0F)) * s;
      const float x = chroma * (1.0F - std::fabs(std::fmod(h, 2.0F) - 1.0F));
      const float m = l - chroma / 2.0F;
      float rgb[3] = {};
      switch (static_cast<int>(h)) {
        case 0: rgb[0] = chroma; rgb[1] = x; break;
        case 1: rgb[0] = x; rgb[1] = chroma; break;
        case 2: rgb[1] = chroma; rgb[2] = x; break;
        case 3: rgb[1] = x; rgb[2] = chroma; break;
        case 4: rgb[0] = x; rgb[2] = chroma; break;
        default: rgb[0] = chroma; rgb[2] = x; break;
      }
      const auto c = [m](float v) { return static_cast<int>(std::lround((v + m) * 255.0F)); };
      return pack(c(rgb[0]), c(rgb[1]), c(rgb[2]));
    }

    void append(std::vector<std::uint8_t>& out, std::string_view s) { out.insert(out.end(), s.begin(), s.end()); }

    void base64(std::span<const std::uint8_t> in, std::string& out) {
      static constexpr char kDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      out.reserve(out.size() + (in.size() + 2) / 3 * 4);
      std::size_t i = 0;
      for (; i + 3 <= in.size(); i += 3) {
        const std::uint32_t v = static_cast<std::uint32_t>(in[i]) << 16 | static_cast<std::uint32_t>(in[i + 1]) << 8 | in[i + 2];
        out += {kDigits[v >> 18], kDigits[(v >> 12) & 63], kDigits[(v >> 6) & 63], kDigits[v & 63]};
      }
      if (const std::size_t rest = in.size() - i; rest > 0) {
        const std::uint32_t v = static_cast<std::uint32_t>(in[i]) << 16 | (rest > 1 ? static_cast<std::uint32_t>(in[i + 1]) << 8 : 0U);
        out += {kDigits[v >> 18], kDigits[(v >> 12) & 63], rest > 1 ? kDigits[(v >> 6) & 63] : '=', '='};
      }
    }

  } // namespace

  SixelDecoder::SixelDecoder() {
    // The VT340 defaults for registers 0-15.
    static constexpr int kDefaults[16][3] = {{0, 0, 0},    {20, 20, 80}, {80, 13, 13}, {20, 80, 20},
                                             {80, 20, 80}, {20, 80, 80}, {80, 80, 20}, {53, 53, 53},
                                             {26, 26, 26}, {33, 33, 60}, {60, 26, 26}, {33, 60, 33},
                                             {60, 33, 60}, {33, 60, 60}, {60, 60, 33}, {80, 80, 80}};
    for (std::size_t i = 0; i < 16; ++i) {
      m_palette[i] = percent(kDefaults[i][0], kDefaults[i][1], kDefaults[i][2]);
    }
    for (std::size_t i = 16; i < m_palette.size(); ++i) {
      m_palette[i] = pack(0, 0, 0);
    }
    m_color = m_palette[0];
  }

  void SixelDecoder::finishCommand() {
    const auto p = [this](std::size_t i) { return i < m_paramCount ? m_params[i] : 0; };
    if (m_command == Command::Color) {
      const auto reg = static_cast<std::size_t>(p(0)) % m_palette.size();
      if (m_paramCount >= 5 && p(1) == 2) {
        m_palette[reg] = percent(p(2), p(3), p(4));
      } else if (m_paramCount >= 5 && p(1) == 1) {
        m_palette[reg] = hls(p(2), p(3), p(4));
      }
      m_color = m_palette[reg];
    } else if (m_command == Command::Raster && m_paramCount >= 4) {
      m_rasterWidth = std::min(p(2), kMaxSide);
      m_rasterHeight = std::min(p(3), kMaxSide);
    }
    m_command = Command::None;
  }

  void SixelDecoder::draw(std::uint8_t bits, int count) {
    const int end = std::min(m_x + count, kMaxSide);
    for (int i = 0; i < 6 && bits != 0; ++i) {
      const int y = m_band + i;
      if ((bits & (1U << i)) == 0 || y >= kMaxSide || m_x >= end) {
        continue;
      }
      if (m_rows.size() <= static_cast<std::size_t>(y)) {
        m_rows.resize(static_cast<std::size_t>(y) + 1);
      }
      auto& row = m_rows[static_cast<std::size_t>(y)];
      if (row.size() < static_cast<std::size_t>(end)) {
        row.resize(static_cast<std::size_t>(end));
      }
      std::fill(row.begin() + m_x, row.begin() + end, m_color);
    }
    m_x = end;
  }

  void SixelDecoder::feed(std::span<const std::uint8_t> data) {
    for (const std::uint8_t b : data) {
      if (m_command != Command::None && b >= '0' && b <= '9') {
        int& v = m_params[m_paramCount - 1];
        v = std::min(v * 10 + (b - '0'), kMaxParam);
        continue;
      }
      if (m_command != Command::None && b == ';') {
        if (m_paramCount < m_params.size()) {
          m_params[m_paramCount++] = 0;
        }
        continue;
      }
      const bool sixel = b >= 0x3F && b <= 0x7E;
      if (m_command == Command::Repeat && sixel) {
        m_command = Command::None;
        draw(static_cast<std::uint8_t>(b - 0x3F), std::max(1, m_params[0]));
        continue;
      }
      finishCommand();
      switch (b) {
        case '!': m_command = Command::Repeat; break;
        case '#': m_command = Command::Color; break;
        case '"': m_command = Command::Raster; break;
        case '$': m_x = 0; break;
        case '-':
          m_x = 0;
          m_band = std::min(m_band + 6, kMaxSide);
          break;
        default:
          if (sixel) {
            draw(static_cast<std::uint8_t>(b - 0x3F), 1);
          }
          break;
      }
      if (m_command != Command::None) {
        m_params = {};
        m_paramCount = 1;
      }
    }
  }

  int SixelDecoder::width() const noexcept {
    std::size_t w = static_cast<std::size_t>(m_rasterWidth);
    for (const auto& row : m_rows) {
      w = std::max(w, row.size());
    }
    return static_cast<int>(w);
  }

  int SixelDecoder::height() const noexcept {
    return std::max(m_rasterHeight, static_cast<int>(m_rows.size()));
  }

  std::vector<std::uint8_t> SixelDecoder::rgba() const {
    const auto w = static_cast<std::size_t>(width());
    std::vector<std::uint8_t> out(w * static_cast<std::size_t>(height()) * 4, 0);
    for (std::size_t y = 0; y < m_rows.size(); ++y) {
      for (std::size_t x = 0; x < m_rows[y].size(); ++x) {
        const std::uint32_t c = m_rows[y][x];
        std::uint8_t* px = out.data() + (y * w + x) * 4;
        px[0] = static_cast<std::uint8_t>(c);
        px[1] = static_cast<std::uint8_t>(c >> 8);
        px[2] = static_cast<std::uint8_t>(c >> 16);
        px[3] = static_cast<std::uint8_t>(c >> 24);
      }
    }
    return out;
  }

  void SixelFilter::flush(std::vector<std::uint8_t>& out) {
    append(out, m_held);
    m_held.clear();
    m_state = State::Ground;
  }

  void SixelFilter::finish(std::vector<std::uint8_t>& out) {
    const SixelDecoder& image = *m_decoder;
    const int w = image.width();
    const int h = image.height();
    if (w > 0 && h > 0) {
      std::string data;
      base64(image.rgba(), data);
      // C=0 leaves the cursor right of the image on its last row; CR LF starts the line below.
      for (std::size_t at = 0; at < data.size(); at += kChunk) {
        const bool more = at + kChunk < data.size();
        append(out, at == 0 ? "\x1b_Ga=T,f=32,q=2,s=" + std::to_string(w) + ",v=" + std::to_string(h) + ",m=" : "\x1b_Gm=");
        append(out, more ? "1;" : "0;");
        append(out, std::string_view(data).substr(at, kChunk));
        append(out, "\x1b\\");
      }
      append(out, "\r\n");
    }
    m_decoder.reset();
    m_state = State::Ground;
  }

  void SixelFilter::feed(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out) {
    for (std::size_t at = 0; at < in.size(); ++at) {
      const std::uint8_t b = in[at];
      switch (m_state) {
        case State::Ground: {
          if (b == kEsc) {
            m_held.assign(1, static_cast<char>(b));
            m_state = State::Introducer;
            break;
          }
          const auto* esc = static_cast<const std::uint8_t*>(std::memchr(in.data() + at, kEsc, in.size() - at));
          const std::size_t end = esc != nullptr ? static_cast<std::size_t>(esc - in.data()) : in.size();
          out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(at), in.begin() + static_cast<std::ptrdiff_t>(end));
          at = end - 1;
          break;
        }
        case State::Introducer:
          if (b == 'P') {
            m_held.push_back('P');
            m_state = State::Params;
          } else {
            flush(out);
            --at;
          }
          break;
        case State::Params:
          if (b == 'q') {
            m_held.clear();
            m_decoder.emplace();
            m_state = State::Data;
          } else if (((b >= '0' && b <= '9') || b == ';') && m_held.size() < kMaxParams) {
            m_held.push_back(static_cast<char>(b));
          } else {
            flush(out);
            --at;
          }
          break;
        case State::Data: {
          std::size_t end = at;
          while (end < in.size() && in[end] != kEsc && in[end] != kCan && in[end] != kSub) {
            ++end;
          }
          m_decoder->feed(in.subspan(at, end - at));
          if (end == in.size()) {
            at = end - 1;
          } else if (in[end] == kEsc) {
            m_state = State::DataEscape;
            at = end;
          } else {
            m_decoder.reset(); // CAN or SUB cancels the image
            m_state = State::Ground;
            at = end - 1;
          }
          break;
        }
        case State::DataEscape:
          if (b == '\\') {
            finish(out);
          } else {
            m_decoder.reset(); // ESC ends the sequence unfinished; it starts a new one
            m_held.assign(1, static_cast<char>(kEsc));
            m_state = State::Introducer;
            --at;
          }
          break;
      }
    }
  }

} // namespace term
