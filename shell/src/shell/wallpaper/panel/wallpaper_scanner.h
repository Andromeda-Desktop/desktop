#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct WallpaperEntry {
  std::string name;
  std::filesystem::path absPath;
  bool isDir = false;
  // Cached on the scan worker so date sorting never stats per comparison.
  std::filesystem::file_time_type mtime;
  bool hasMtime = false;
};

struct WallpaperScanResult {
  std::string dir;
  std::vector<WallpaperEntry> entries;
  std::filesystem::file_time_type dirMtime;
  bool flatten = false;
};

// Walks wallpaper directories on BlockingPool, so the per-entry stat calls never run on the UI thread. Results land
// in a main-thread cache and fire onComplete().
class WallpaperScanner {
public:
  // Fired on the main thread after a scan lands in the cache.
  using CompletionCallback = std::function<void()>;

  WallpaperScanner() = default;

  WallpaperScanner(const WallpaperScanner&) = delete;
  WallpaperScanner& operator=(const WallpaperScanner&) = delete;

  void setOnComplete(CompletionCallback callback) { m_onComplete = std::move(callback); }

  // Ensures (dir, flatten) is cached and fresh against the directory's mtime.
  // Returns true when a fresh cached result already exists (nothing queued);
  // returns false when a worker scan was queued — onComplete() fires later.
  // A missing/empty directory caches an empty result and returns true.
  bool requestScan(const std::filesystem::path& dir, bool flatten);

  // Cached result for (dir, flatten), or nullptr. Pure main-thread lookup.
  [[nodiscard]] const WallpaperScanResult* cached(const std::filesystem::path& dir, bool flatten) const;

  // True if a scan for (dir, flatten) is queued or in flight.
  [[nodiscard]] bool scanning(const std::filesystem::path& dir, bool flatten) const;

  // Drops all cached results so the next requestScan() re-walks.
  void invalidate();

private:
  struct CacheKey {
    std::string dir;
    bool flatten = false;
    bool operator==(const CacheKey& other) const noexcept { return flatten == other.flatten && dir == other.dir; }
  };
  struct CacheKeyHash {
    std::size_t operator()(const CacheKey& k) const noexcept {
      return std::hash<std::string>{}(k.dir) ^ (k.flatten ? 0x9e3779b9U : 0U);
    }
  };

  void apply(WallpaperScanResult result);

  std::unordered_map<CacheKey, WallpaperScanResult, CacheKeyHash> m_cache;
  std::unordered_set<CacheKey, CacheKeyHash> m_pending;
  CompletionCallback m_onComplete;
  // Scans finish on the main loop only while the scanner lives.
  std::shared_ptr<int> m_alive = std::make_shared<int>(0);
};
