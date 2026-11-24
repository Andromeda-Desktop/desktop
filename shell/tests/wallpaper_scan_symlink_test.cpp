#include "shell/wallpaper/panel/wallpaper_scanner.h"
#include "tests/test_deferred.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <vector>

namespace {

  namespace fs = std::filesystem;

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "wallpaper_scan_symlink_test: {}", message);
      return false;
    }
    return true;
  }

  void touch(const fs::path& path) {
    std::ofstream out(path);
    out << "x";
  }

  std::size_t countNamed(const std::vector<WallpaperEntry>& entries, std::string_view name) {
    return static_cast<std::size_t>(std::ranges::count_if(entries, [name](const WallpaperEntry& e) {
      return e.name == name;
    }));
  }

  // Runs the scan to completion through the main-loop callbacks, the same way the shell drives the scanner. Returns
  // nullptr if no result lands before the deadline.
  const WallpaperScanResult* scanBlocking(WallpaperScanner& scanner, const fs::path& dir) {
    if (scanner.requestScan(dir, true)) {
      return scanner.cached(dir, true);
    }
    runDeferredUntil([&] { return scanner.cached(dir, true) != nullptr; }, std::chrono::seconds(20));
    return scanner.cached(dir, true);
  }

} // namespace

int main() {
  const fs::path tempDir = fs::temp_directory_path()
      / ("noctalia-wallpaper-scan-symlink-"
         + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const fs::path root = tempDir / "root";
  const fs::path local = root / "local";
  const fs::path external = tempDir / "external";
  fs::create_directories(local);
  fs::create_directories(external / "nested");

  touch(local / "local.png");
  touch(external / "external.png");
  touch(external / "nested" / "nested.png");

  // The reported setup: a directory symlink sitting next to a real one.
  fs::create_directory_symlink(external, root / "linked");
  // A symlink pointing back at the scan root, which loops without a guard.
  fs::create_directory_symlink(root, external / "nested" / "loop");
  // A second symlink to the same target, which duplicates without a guard.
  fs::create_directory_symlink(external, root / "linked-again");

  bool ok = true;
  {
    WallpaperScanner scanner;
    const WallpaperScanResult* result = scanBlocking(scanner, root);
    ok = expect(result != nullptr, "flatten scan did not complete") && ok;
    if (result != nullptr) {
      ok = expect(countNamed(result->entries, "local.png") == 1, "local image missing or duplicated") && ok;
      ok = expect(countNamed(result->entries, "external.png") == 1, "symlinked image missing or duplicated") && ok;
      ok = expect(countNamed(result->entries, "nested.png") == 1, "image under symlinked subdirectory missing") && ok;
      ok = expect(result->entries.size() == 3, "flatten scan returned unexpected entries") && ok;
    }
  }

  std::error_code ec;
  fs::remove_all(tempDir, ec);
  if (!ok) {
    return 1;
  }
  std::println("wallpaper_scan_symlink_test: ok");
  return 0;
}
