#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace term {

  // Finds `ESC ] <prefix> <payload> (BEL | ESC \)` in a byte stream that libghostty also parses. Bytes may split a
  // sequence anywhere; state carries over to the next call. A payload over `maxPayload` bytes is dropped.
  class OscFrame {
  public:
    OscFrame(std::string_view prefix, std::size_t maxPayload) : m_prefix(prefix), m_maxPayload(maxPayload) {}

    void feed(std::span<const std::uint8_t> bytes, const std::function<void(std::string_view payload)>& done);

  private:
    enum class State : std::uint8_t { Ground, Escape, Prefix, Payload, PayloadEscape, Skip, SkipEscape };

    std::string_view m_prefix;
    std::size_t m_maxPayload;
    State m_state = State::Ground;
    std::size_t m_prefixAt = 0;
    std::string m_payload;
  };

  // Standard base64; skips characters outside the alphabet.
  [[nodiscard]] std::string base64Decode(std::string_view in);

} // namespace term
