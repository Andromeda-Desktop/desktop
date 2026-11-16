#include "sixel.h"

#include <cstdio>
#include <random>
#include <string>

using namespace term;

namespace {

int failures = 0;
unsigned seed = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s (seed %u)\n", __FILE__, __LINE__, #c, seed); ++failures; } } while (0)

std::string run(const std::string& in, std::mt19937& rng) {
  SixelFilter filter;
  std::vector<std::uint8_t> out;
  for (std::size_t at = 0; at < in.size();) {
    const std::size_t len = std::min(in.size() - at, std::uniform_int_distribution<std::size_t>(1, 9)(rng));
    filter.feed({reinterpret_cast<const std::uint8_t*>(in.data() + at), len}, out);
    at += len;
  }
  return {out.begin(), out.end()};
}

SixelDecoder decode(const std::string& data) {
  SixelDecoder d;
  d.feed({reinterpret_cast<const std::uint8_t*>(data.data()), data.size()});
  return d;
}

std::uint32_t pixel(const SixelDecoder& d, int x, int y) {
  const auto rgba = d.rgba();
  const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(d.width()) + static_cast<std::size_t>(x)) * 4;
  return static_cast<std::uint32_t>(rgba[at]) << 24 | static_cast<std::uint32_t>(rgba[at + 1]) << 16
         | static_cast<std::uint32_t>(rgba[at + 2]) << 8 | rgba[at + 3];
}

} // namespace

int main() {
  std::random_device rd;
  seed = rd();
  std::mt19937 rng(seed);

  // Register 1 red, 3 columns of full bands (~), then on the next band 2 repeated columns of the top pixel (@) in green.
  const SixelDecoder d = decode("#1;2;100;0;0#1~~~-#2;2;0;100;0!2@");
  CHECK(d.width() == 3 && d.height() == 7);
  CHECK(pixel(d, 0, 0) == 0xFF0000FF && pixel(d, 2, 5) == 0xFF0000FF);
  CHECK(pixel(d, 1, 6) == 0x00FF00FF);
  CHECK(pixel(d, 2, 6) == 0); // never drawn: transparent
  // HLS: hue 120 is red in DEC's wheel.
  CHECK(pixel(decode("#5;1;120;50;100#5?@"), 1, 0) == 0xFF0000FF);
  // Raster attributes size the image even where nothing is drawn; drawing past the limit is dropped.
  const SixelDecoder raster = decode("\"1;1;10;12~");
  CHECK(raster.width() == 10 && raster.height() == 12);
  const SixelDecoder huge = decode("!99999999~");
  CHECK(huge.width() == SixelDecoder::kMaxSide && huge.height() == 6);

  for (int i = 0; i < 200; ++i) {
    // A sixel image becomes one kitty image at the cursor, then the cursor goes to the next line.
    const std::string out = run("a\x1bP0;1;0q\"1;1;2;6#1~~\x1b\\b", rng);
    CHECK(out.starts_with("a\x1b_Ga=T,f=32,q=2,s=2,v=6,m=0;"));
    CHECK(out.ends_with("\x1b\\\r\nb"));
    // Other DCS sequences, a cancelled image and plain escapes pass through unchanged.
    const std::string other = "\x1bP$qm\x1b\\\x1bP+q544e\x1b\\\x1b[31mx\x1bPq#1~\x18y\x1b";
    const std::string passed = run(other, rng);
    CHECK(passed == "\x1bP$qm\x1b\\\x1bP+q544e\x1b\\\x1b[31mx\x18y");
  }

  if (failures == 0) {
    std::puts("sixel: ok");
  }
  return failures == 0 ? 0 : 1;
}
