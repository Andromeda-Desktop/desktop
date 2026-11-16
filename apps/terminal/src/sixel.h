#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace term {

  // Decodes sixel data (the bytes after DCS P1;P2;P3 q, up to ST) into RGBA, in pieces of any size.
  class SixelDecoder {
  public:
    static constexpr int kMaxSide = 4096; // pixels; drawing past it is dropped

    SixelDecoder();
    void feed(std::span<const std::uint8_t> data);
    // Size: the drawn extent, at least the raster attributes' size. Undrawn pixels stay transparent.
    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;
    // Row-major RGBA, width() * height() * 4 bytes.
    [[nodiscard]] std::vector<std::uint8_t> rgba() const;

  private:
    enum class Command : std::uint8_t { None, Repeat, Color, Raster };
    void finishCommand();
    void draw(std::uint8_t bits, int count);

    std::array<std::uint32_t, 256> m_palette{}; // 0xAABBGGRR, as RGBA bytes in memory order
    std::vector<std::vector<std::uint32_t>> m_rows;
    std::uint32_t m_color = 0;
    int m_x = 0;
    int m_band = 0; // top pixel row of the current band of 6
    int m_rasterWidth = 0;
    int m_rasterHeight = 0;
    Command m_command = Command::None;
    std::array<int, 5> m_params{};
    std::size_t m_paramCount = 0;
  };

  // Rewrites each sixel sequence (DCS [params] q ... ST) before libghostty, which has no sixel support, into a kitty
  // graphics image (RGBA, direct, chunked) placed at the cursor; the cursor ends at the start of the line below it.
  // Other DCS sequences pass unchanged.
  class SixelFilter {
  public:
    // Appends `in`, rewritten, to `out`. Bytes may split a sequence anywhere; state carries over.
    void feed(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out);

  private:
    enum class State : std::uint8_t { Ground, Introducer, Params, Data, DataEscape };
    void flush(std::vector<std::uint8_t>& out);
    void finish(std::vector<std::uint8_t>& out);

    State m_state = State::Ground;
    std::string m_held; // ESC P and its parameters, sent unchanged when the sequence is not sixel
    std::optional<SixelDecoder> m_decoder; // inside a sixel sequence
  };

} // namespace term
