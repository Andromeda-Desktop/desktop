#include "osc66.h"

#include <cstdio>
#include <random>
#include <string>

using namespace term;

namespace {

int failures = 0;
unsigned seed = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s (seed %u)\n", __FILE__, __LINE__, #c, seed); ++failures; } } while (0)

std::string run(TextSizeFilter& filter, const std::string& in, std::mt19937& rng) {
  std::vector<std::uint8_t> out;
  for (std::size_t at = 0; at < in.size();) {
    const std::size_t len = std::min(in.size() - at, std::uniform_int_distribution<std::size_t>(1, 9)(rng));
    filter.feed({reinterpret_cast<const std::uint8_t*>(in.data() + at), len}, out);
    at += len;
  }
  return {out.begin(), out.end()};
}

std::string feedAll(const std::string& in) {
  TextSizeFilter filter;
  std::vector<std::uint8_t> out;
  filter.feed({reinterpret_cast<const std::uint8_t*>(in.data()), in.size()}, out);
  return {out.begin(), out.end()};
}

} // namespace

int main() {
  std::random_device rd;
  seed = rd();
  std::mt19937 rng(seed);
  const std::string link = "\x1b]8;;andromeda-text-size:";
  const std::string end = "\x1b]8;;\x1b\\";

  // Invariant: output without OSC 66 is the input, whatever the chunking.
  const char* pieces[] = {"ab", "\x1b[1;31m", "\x1b]0;title\x07", "\x1b]6;x\x07", "\x1b]66", "\x1b\x1b]", "日本",
                          "\x1b]8;;http://x\x1b\\", "\x1b", "\n"};
  for (int i = 0; i < 300; ++i) {
    std::string in;
    for (int k = 0; k < 20; ++k) {
      in += pieces[std::uniform_int_distribution<int>(0, 9)(rng)];
    }
    if (in.find("\x1b]66;") != std::string::npos) {
      continue;
    }
    TextSizeFilter filter;
    const std::string out = run(filter, in, rng);
    // A trailing partial "ESC ] 6 6" stays held until more bytes come.
    CHECK(in.starts_with(out) && in.size() - out.size() <= 4);
  }

  // Invariant: a sized run becomes its text, padded to w*s cells (or s times its own width), inside the size link;
  // any chunking gives the same bytes.
  const std::string in = "a\x1b]66;s=2;hi\x07" "b\x1b]66;w=3:s=2;日\x1b\\c\x1b]66;s=9;x\x07";
  const std::string want = "a" + link + "0:s=2\x1b\\hi  " + end + "b" + link + "1:w=3:s=2\x1b\\日    " + end + "c\x1b]66;s=9;x\x07";
  CHECK(feedAll(in) == want);
  for (int i = 0; i < 100; ++i) {
    TextSizeFilter filter;
    CHECK(run(filter, in, rng) == want);
  }

  CHECK(textSizeOfUri("andromeda-text-size:4:s=3:v=1") == (TextSize{.s = 3, .v = 1}));
  CHECK(textSizeOfUri("http://andromeda-text-size:4:s=3") == std::nullopt);
  CHECK(textCells("a日́😀") == 5);

  if (failures != 0) {
    std::fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  std::puts("osc66: ok");
  return 0;
}
