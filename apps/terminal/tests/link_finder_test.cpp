#include "link_finder.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
  int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

  struct Piece {
    std::u32string text;
    std::string uri; // expected link for every column of text; empty: no link
  };

  std::u32string wide(const std::string& s) { return {s.begin(), s.end()}; }
} // namespace

// Random rows of words, URLs, existing and missing paths with :line:col suffixes, punctuation and wide characters.
// Every column must yield exactly the link of the piece it lies in.
int main() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / ("link-finder-" + std::to_string(::getpid()));
  fs::create_directories(dir / "src");
  std::ofstream(dir / "src" / "main.cpp") << "x";
  const std::string cwd = dir.string();
  const std::string mainUri = "file://" + cwd + "/src/main.cpp";

  const std::vector<Piece> pieces = {
      {U"hello", ""},
      {U"src/main.cpp", mainUri},
      {U"src/main.cpp:42", mainUri},
      {U"src/main.cpp:42:7", mainUri},
      {wide(cwd) + U"/src/main.cpp", mainUri},
      {wide(cwd) + U"/src", "file://" + cwd + "/src"},
      {U"src/missing.cpp:3", ""},
      {U"https://example.org/a?b=1#c", "https://example.org/a?b=1#c"},
      {U"http://x.y/z", "http://x.y/z"},
      {U"ftp://x.y/z", ""},
      {U"v1.2:3", ""},
      {std::u32string(U"中\0", 2), ""}, // a wide character and its spacer
  };
  const std::vector<std::u32string> gaps = {U" ", U"  ", U" (", U") ", U", ", U" \"", U"\" ", U": "};

  std::mt19937 rng(12345);
  for (int round = 0; round < 2000; ++round) {
    std::u32string row;
    std::vector<std::string> expected;
    const int count = 1 + static_cast<int>(rng() % 5);
    for (int i = 0; i < count; ++i) {
      const std::u32string& gap = gaps[rng() % gaps.size()];
      row += gap;
      expected.insert(expected.end(), gap.size(), "");
      const Piece& p = pieces[rng() % pieces.size()];
      row += p.text;
      expected.insert(expected.end(), p.text.size(), p.uri);
    }
    for (int col = 0; col < static_cast<int>(row.size()); ++col) {
      const auto link = term::findLink(row, col, cwd);
      const std::string got = link ? link->uri : "";
      if (got != expected[static_cast<std::size_t>(col)]) {
        std::fprintf(stderr, "round %d col %d: got '%s' want '%s'\n", round, col, got.c_str(),
                     expected[static_cast<std::size_t>(col)].c_str());
        ++failures;
        break;
      }
      if (link) {
        CHECK(link->start <= col && col < link->end);
        for (int c = link->start; c < link->end; ++c) {
          CHECK(expected[static_cast<std::size_t>(c)] == got);
        }
      }
    }
    if (failures > 0) {
      break;
    }
  }

  CHECK(!term::findLink(U"src/main.cpp", 0, "").has_value());
  std::error_code ec;
  fs::remove_all(dir, ec);
  return failures == 0 ? 0 : 1;
}
