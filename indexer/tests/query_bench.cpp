// Name query latency on 100 k synthetic files: prints p50, p95 and max over typed prefixes of real names.
#include "crawler.h"
#include "index_db.h"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace andromeda::index;

int main() {
  constexpr int kFolders = 400;
  constexpr int kPerFolder = 250;
  constexpr std::array<const char*, 24> kWords = {
      "report", "invoice", "budget", "photo", "holiday", "notes", "draft", "final", "meeting", "scan", "resume", "thesis",
      "chapter", "backup", "project", "design", "summary", "contract", "receipt", "slides", "recipe", "travel", "lecture",
      "screenshot"};
  constexpr std::array<const char*, 8> kExts = {".pdf", ".txt", ".png", ".jpg", ".md", ".odt", ".mp3", ".cpp"};
  const fs::path root = fs::temp_directory_path() / ("query-bench-" + std::to_string(getpid()));
  std::mt19937 rng(42);
  std::vector<std::string> names;
  for (int f = 0; f < kFolders; ++f) {
    const fs::path folder = root / kWords[f % kWords.size()] / ("set-" + std::to_string(f));
    fs::create_directories(folder);
    for (int i = 0; i < kPerFolder; ++i) {
      std::string name = std::string(kWords[rng() % kWords.size()]) + "-" + kWords[rng() % kWords.size()] + "-"
          + std::to_string(rng() % 10000) + kExts[rng() % kExts.size()];
      const std::ofstream file(folder / name);
      names.push_back(std::move(name));
    }
  }
  {
    IndexDb bulk(root / "index.db", true);
    crawl(bulk, root);
    bulk.finishBulk();
  }
  const IndexDb db(root / "index.db");
  const auto loadStart = std::chrono::steady_clock::now();
  db.loadNames();
  std::printf("load ms: %.1f\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - loadStart).count());
  std::printf("%lld entries\n", static_cast<long long>(db.count()));

  // What a user types, three ways: 3 to 12 leading characters of a name or of its second word; 1 or 2 leading
  // characters; and a fragment with every other letter left out, which takes the letters-in-order pass.
  const auto run = [&](const char* label, auto makeQuery) {
    std::vector<double> ms;
    for (int q = 0; q < 2000; ++q) {
      const std::string& name = names[rng() % names.size()];
      const std::size_t from = (q % 2 == 0) ? 0 : name.find('-') + 1;
      const std::string query = makeQuery(name.substr(from));
      const auto start = std::chrono::steady_clock::now();
      const auto rows = db.queryNames(query, 20);
      ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::ranges::sort(ms);
    const double p95 = ms[ms.size() * 95 / 100];
    std::printf("%s query ms: p50 %.2f  p95 %.2f  max %.2f\n", label, ms[ms.size() / 2], p95, ms.back());
    return p95 < 5.0;
  };
  const bool typed = run("typed", [&](const std::string& word) { return word.substr(0, 3 + rng() % 10); });
  const bool shortQuery = run("short", [&](const std::string& word) { return word.substr(0, 1 + rng() % 2); });
  const bool gapped = run("gapped", [&](const std::string& word) {
    std::string out;
    for (std::size_t i = 0; i < std::min<std::size_t>(word.size(), 12); i += 2) {
      out += word[i];
    }
    return out.size() >= 3 ? out : word.substr(0, 3);
  });
  fs::remove_all(root);
  return typed && shortQuery && gapped ? 0 : 1;
}
