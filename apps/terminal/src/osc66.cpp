#include "osc66.h"

#include <algorithm>
#include <charconv>
#include <cstring>

namespace term {

  namespace {

    constexpr std::uint8_t kEsc = 0x1b;
    constexpr std::uint8_t kBel = 0x07;
    constexpr std::uint8_t kCan = 0x18;
    constexpr std::uint8_t kSub = 0x1a;
    constexpr std::string_view kPrefix = "\x1b]66;";
    constexpr std::size_t kMaxPayload = 4096 + 64; // the protocol's 4096 bytes of text plus the keys

    void append(std::vector<std::uint8_t>& out, std::string_view text) { out.insert(out.end(), text.begin(), text.end()); }

    bool wide(char32_t cp) {
      return (cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0xa4cf && cp != 0x303f) || (cp >= 0xac00 && cp <= 0xd7a3)
             || (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xfe30 && cp <= 0xfe4f) || (cp >= 0xff00 && cp <= 0xff60)
             || (cp >= 0xffe0 && cp <= 0xffe6) || (cp >= 0x1f300 && cp <= 0x1faff) || (cp >= 0x20000 && cp <= 0x3fffd);
    }

    bool zeroWidth(char32_t cp) {
      return (cp >= 0x300 && cp <= 0x36f) || cp == 0x200b || cp == 0x200c || cp == 0x200d || (cp >= 0xfe00 && cp <= 0xfe0f)
             || (cp >= 0x1ab0 && cp <= 0x1aff) || (cp >= 0x20d0 && cp <= 0x20ff);
    }

  } // namespace

  std::optional<TextSize> parseTextSize(std::string_view params) {
    TextSize size;
    while (!params.empty()) {
      const std::size_t colon = params.find(':');
      const std::string_view pair = params.substr(0, colon);
      params = colon == std::string_view::npos ? std::string_view{} : params.substr(colon + 1);
      if (pair.size() < 3 || pair[1] != '=') {
        return std::nullopt;
      }
      int value = 0;
      const auto [end, ec] = std::from_chars(pair.data() + 2, pair.data() + pair.size(), value);
      if (ec != std::errc{} || end != pair.data() + pair.size()) {
        return std::nullopt;
      }
      const auto set = [value](int& field, int max) {
        field = value;
        return value >= 0 && value <= max;
      };
      bool ok = false;
      switch (pair[0]) {
        case 's': ok = set(size.s, 7) && value >= 1; break;
        case 'w': ok = set(size.w, 7); break;
        case 'n': ok = set(size.n, 15); break;
        case 'd': ok = set(size.d, 15); break;
        case 'v': ok = set(size.v, 2); break;
        case 'h': ok = set(size.h, 2); break;
        default: break;
      }
      if (!ok) {
        return std::nullopt;
      }
    }
    return size;
  }

  std::optional<TextSize> textSizeOfUri(std::string_view uri) {
    if (!uri.starts_with(kTextSizeScheme)) {
      return std::nullopt;
    }
    uri.remove_prefix(kTextSizeScheme.size());
    const std::size_t colon = uri.find(':'); // the run number
    return colon == std::string_view::npos ? parseTextSize({}) : parseTextSize(uri.substr(colon + 1));
  }

  int textCells(std::string_view utf8) {
    int cells = 0;
    for (std::size_t i = 0; i < utf8.size();) {
      const auto b = static_cast<unsigned char>(utf8[i]);
      const int len = b < 0x80 ? 1 : b < 0xe0 ? 2 : b < 0xf0 ? 3 : 4;
      char32_t cp = len == 1 ? b : b & (0x3fU >> (len - 1));
      for (int k = 1; k < len && i + static_cast<std::size_t>(k) < utf8.size(); ++k) {
        cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + static_cast<std::size_t>(k)]) & 0x3fU);
      }
      i += static_cast<std::size_t>(len);
      cells += cp < 0x20 || zeroWidth(cp) ? 0 : wide(cp) ? 2 : 1;
    }
    return cells;
  }

  void TextSizeFilter::flush(std::vector<std::uint8_t>& out) {
    append(out, m_held);
    m_held.clear();
    m_state = State::Ground;
  }

  void TextSizeFilter::finish(std::vector<std::uint8_t>& out, std::string_view terminator) {
    const std::string_view payload = std::string_view(m_held).substr(kPrefix.size(), m_payloadAt);
    const std::size_t semi = payload.find(';');
    const auto size = semi == std::string_view::npos ? std::nullopt : parseTextSize(payload.substr(0, semi));
    if (!size) {
      flush(out); // libghostty drops it as before
      append(out, terminator);
      return;
    }
    const std::string_view text = payload.substr(semi + 1);
    const int natural = textCells(text);
    const int cells = size->w > 0 ? size->w * size->s : natural * size->s;
    append(out, "\x1b]8;;");
    append(out, kTextSizeScheme);
    append(out, std::to_string(m_runs++) + ":" + std::string(payload.substr(0, semi)));
    append(out, "\x1b\\");
    append(out, text);
    append(out, std::string(static_cast<std::size_t>(std::max(0, cells - natural)), ' '));
    append(out, "\x1b]8;;\x1b\\");
    m_held.clear();
    m_state = State::Ground;
  }

  void TextSizeFilter::feed(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out) {
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
              m_payloadAt = 0;
              m_state = State::Payload;
            }
          } else {
            flush(out);
            --at; // b starts over in Ground
          }
          break;
        case State::Payload:
          if (b == kBel) {
            finish(out, "\x07");
          } else if (b == kEsc) {
            m_state = State::PayloadEscape;
          } else if (b == kCan || b == kSub || m_payloadAt >= kMaxPayload) {
            flush(out);
            out.push_back(b);
          } else {
            m_held.push_back(static_cast<char>(b));
            ++m_payloadAt;
          }
          break;
        case State::PayloadEscape:
          if (b == '\\') {
            finish(out, "\x1b\\");
          } else {
            flush(out); // ESC ends the OSC; it starts a new sequence
            m_held.assign(1, static_cast<char>(kEsc));
            m_state = State::Prefix;
            --at;
          }
          break;
      }
    }
  }

} // namespace term
