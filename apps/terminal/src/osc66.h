#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace term {

  // Kitty text sizing (OSC 66 ; s=..:w=..:n=..:d=..:v=..:h=.. ; text): s = scale in cells, w = width in scaled
  // cells (0: the text's own), n/d = fractional scale, v/h = alignment in the block.
  struct TextSize {
    int s = 1;
    int w = 0;
    int n = 0;
    int d = 0;
    int v = 0;
    int h = 0;
    bool operator==(const TextSize&) const = default;
  };

  // libghostty parses OSC 66 and drops it, text included. The filter rewrites each sequence before libghostty sees
  // it: the text, padded to the block's width, inside an OSC 8 link to kTextSizeScheme, so the cells hold the text and
  // the size, and scroll, reflow and clear with it. The closing OSC 8 also ends a link the program had open.
  class TextSizeFilter {
  public:
    // Appends `in`, rewritten, to `out`. Bytes may split a sequence anywhere; state carries over.
    void feed(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out);
    bool idle() const { return m_state == State::Ground; }

  private:
    enum class State : std::uint8_t { Ground, Prefix, Payload, PayloadEscape };
    void finish(std::vector<std::uint8_t>& out, std::string_view terminator);
    void flush(std::vector<std::uint8_t>& out);

    State m_state = State::Ground;
    std::string m_held; // bytes of a sequence that may still be OSC 66, sent unchanged when it is not
    std::size_t m_payloadAt = 0;
    std::uint32_t m_runs = 0; // numbers each run, so adjacent runs with the same size stay apart
  };

  inline constexpr std::string_view kTextSizeScheme = "andromeda-text-size:";
  // The size in a cell's link URI; nullopt for any other link.
  [[nodiscard]] std::optional<TextSize> textSizeOfUri(std::string_view uri);
  // Parses "s=2:w=3"; nullopt when a key or value is out of the protocol's range.
  [[nodiscard]] std::optional<TextSize> parseTextSize(std::string_view params);
  // Terminal cells of UTF-8 text at scale 1 (wide CJK and emoji 2, combining marks 0).
  [[nodiscard]] int textCells(std::string_view utf8);

} // namespace term
