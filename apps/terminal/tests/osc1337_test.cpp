#include "osc1337.h"
#include "osc_frame.h"

#include <cstdio>
#include <random>
#include <string>

using namespace term;

namespace {

int failures = 0;
unsigned seed = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s (seed %u)\n", __FILE__, __LINE__, #c, seed); ++failures; } } while (0)

// 2x2 RGBA, not premultiplied: red, green / blue, transparent white.
const std::string kPng = "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFElEQVR4nGP4z8DwHwwZGP6DAAMASMkI+HUch0MAAAAASUVORK5CYII=";
const CellGrid kGrid{80, 24, 8, 16};

std::string run(const std::string& in, std::mt19937& rng, std::size_t maxChunk = 9) {
  ImageFilter filter;
  std::vector<std::uint8_t> out;
  for (std::size_t at = 0; at < in.size();) {
    const std::size_t len = std::min(in.size() - at, std::uniform_int_distribution<std::size_t>(1, maxChunk)(rng));
    filter.feed({reinterpret_cast<const std::uint8_t*>(in.data() + at), len}, out, kGrid);
    at += len;
  }
  return {out.begin(), out.end()};
}

// The RGBA bytes of every kitty chunk in `out`.
std::string kittyPixels(const std::string& out) {
  std::string data;
  for (std::size_t at = 0; (at = out.find("\x1b_G", at)) != std::string::npos;) {
    const std::size_t semi = out.find(';', at);
    const std::size_t end = out.find("\x1b\\", semi);
    data += out.substr(semi + 1, end - semi - 1);
    at = end;
  }
  return base64Decode(data);
}

std::string file(const std::string& args) { return "\x1b]1337;File=" + args + ":" + kPng + "\x07"; }

} // namespace

int main() {
  std::random_device rd;
  seed = rd();
  std::mt19937 rng(seed);

  // Sizes: cells, px, percent, auto from the other side, fit inside the box, stretch, capped to the grid width.
  const auto size = [](const std::string& args, int w, int h) { return imageTargetSize(parseImageArgs(args), w, h, kGrid); };
  CHECK(size("width=2", 10, 20) == std::pair(16, 32));
  CHECK(size("height=40px", 10, 20) == std::pair(20, 40));
  CHECK(size("width=50%", 100, 100) == std::pair(320, 320));
  CHECK(size("width=100px;height=100px", 50, 25) == std::pair(100, 50));
  CHECK(size("width=100px;height=100px;preserveAspectRatio=0", 50, 25) == std::pair(100, 100));
  CHECK(size("", 2000, 100) == std::pair(640, 32));
  CHECK(!size("width=-3", 10, 10) && !size("width=x", 10, 10));

  for (int i = 0; i < 100; ++i) {
    // An inline image becomes one kitty image at the cursor; the text after it follows unchanged.
    const std::string out = run("a" + file("name=eC5wbmc=;size=75;inline=1") + "b", rng);
    CHECK(out.starts_with("a\x1b_Ga=T,f=32,q=2,s=2,v=2,m=0;"));
    CHECK(out.ends_with("\x1b\\b"));
    CHECK(kittyPixels(out) == std::string("\xff\x00\x00\xff\x00\xff\x00\xff\x00\x00\xff\xff\xff\xff\xff\x00", 16));
    // Scaled up 2x by nearest pixel: the top row is red, red, green, green.
    const std::string big = kittyPixels(run(file("inline=1;width=4px"), rng));
    CHECK(big.size() == 64 && big.substr(4, 4) == std::string("\xff\x00\x00\xff", 4) && big.substr(8, 4) == std::string("\x00\xff\x00\xff", 4));
    // Multipart, ST terminators.
    const std::string parts = "\x1b]1337;MultipartFile=inline=1\x1b\\\x1b]1337;FilePart=" + kPng.substr(0, 30) + "\x1b\\\x1b]1337;FilePart="
                              + kPng.substr(30) + "\x1b\\\x1b]1337;FileEnd\x1b\\c";
    const std::string multi = run(parts, rng);
    CHECK(multi.starts_with("\x1b_Ga=T,f=32,q=2,s=2,v=2,m=0;") && multi.ends_with("\x1b\\c"));
    // Other keys pass unchanged; downloads, bad data, cancelled and unterminated images are dropped.
    const std::string other = "\x1b]1337;SetUserVar=k=dg==\x07\x1b]1337;CurrentDir=/tmp\x1b\\\x1b]133;A\x07";
    CHECK(run(other, rng) == other);
    CHECK(run("a" + file("inline=0") + "b", rng) == "ab");
    CHECK(run("a\x1b]1337;File=inline=1:AAAA\x07" "b", rng) == "ab");
    CHECK(run("a\x1b]1337;File=inline=1:" + kPng + "\x18" "b", rng) == "a\x18" "b");
    CHECK(run("a\x1b]1337;File=inline=1:" + kPng + "\x1b[0m", rng) == "a\x1b[0m");
    CHECK(run("\x1b]1337;FileEnd\x07x", rng) == "x");
  }

  // A payload over the cap is dropped and the stream after it parses again.
  const std::string huge = "\x1b]1337;File=inline=1:" + std::string((64U << 20) + 10, 'A') + "\x07" "z" + file("inline=1");
  const std::string after = run(huge, rng, 1 << 20);
  CHECK(after.starts_with("z\x1b_Ga=T"));

  if (failures == 0) {
    std::puts("osc1337: ok");
  }
  return failures == 0 ? 0 : 1;
}
