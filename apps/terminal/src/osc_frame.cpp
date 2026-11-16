#include "osc_frame.h"

namespace term {

namespace {

constexpr std::uint8_t kEsc = 0x1b;
constexpr std::uint8_t kBel = 0x07;
constexpr std::uint8_t kCan = 0x18;
constexpr std::uint8_t kSub = 0x1a;

} // namespace

std::string base64Decode(std::string_view in) {
  std::string out;
  std::uint32_t acc = 0;
  int bits = 0;
  for (const char c : in) {
    int v = -1;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+') v = 62;
    else if (c == '/') v = 63;
    if (v < 0) continue;
    acc = (acc << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((acc >> bits) & 0xff));
    }
  }
  return out;
}

void OscFrame::feed(std::span<const std::uint8_t> bytes, const std::function<void(std::string_view payload)>& done) {
  for (std::size_t at = 0; at < bytes.size();) {
    const std::uint8_t b = bytes[at];
    bool consumed = true;
    switch (m_state) {
      case State::Ground:
        if (b == kEsc) m_state = State::Escape;
        break;
      case State::Escape:
        if (b == ']') {
          m_state = State::Prefix;
          m_prefixAt = 0;
        } else if (b != kEsc) {
          m_state = State::Ground;
        }
        break;
      case State::Prefix:
        if (b == static_cast<std::uint8_t>(m_prefix[m_prefixAt])) {
          if (++m_prefixAt == m_prefix.size()) {
            m_state = State::Payload;
            m_payload.clear();
          }
        } else {
          m_state = b == kEsc ? State::Escape : State::Ground;
        }
        break;
      case State::Payload:
        if (b == kBel) {
          m_state = State::Ground;
          done(m_payload);
        } else if (b == kEsc) {
          m_state = State::PayloadEscape;
        } else if (b == kCan || b == kSub) {
          m_state = State::Ground;
        } else if (m_payload.size() < m_maxPayload) {
          m_payload.push_back(static_cast<char>(b));
        } else {
          m_state = State::Skip;
        }
        break;
      case State::PayloadEscape:
      case State::SkipEscape:
        if (b == '\\') {
          const bool complete = m_state == State::PayloadEscape;
          m_state = State::Ground;
          if (complete) done(m_payload);
        } else {
          // ESC cancels the OSC and starts a new sequence with this byte.
          m_state = State::Escape;
          consumed = false;
        }
        break;
      case State::Skip:
        if (b == kBel || b == kCan || b == kSub) m_state = State::Ground;
        else if (b == kEsc) m_state = State::SkipEscape;
        break;
    }
    if (consumed) ++at;
  }
}

} // namespace term
