#include "shell/wallpaper/panel/wallpaper_scanner.h"

#include "core/blocking_pool.h"
#include "core/files/directory_scanner.h"
#include "core/log.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace {

  constexpr Logger kLog("wp-scan");

  // Pull the entry's modification time from the cached directory_entry metadata
  // so date sorting later never has to stat again.
  void cacheMtime(const std::filesystem::directory_entry& entry, WallpaperEntry& out) {
    std::error_code ec;
    const auto mtime = entry.last_write_time(ec);
    if (!ec) {
      out.mtime = mtime;
      out.hasMtime = true;
    }
  }

  void collectFlat(const std::filesystem::path& dir, std::vector<WallpaperEntry>& out) {
    FileUtils::walkDirectoryTree(
        dir,
        [](const std::filesystem::directory_entry& entry) {
          return !entry.path().filename().string().starts_with('.');
        },
        [&out](const std::filesystem::directory_entry& entry) {
          if (entry.path().filename().string().starts_with('.')) {
            return;
          }
          std::error_code typeEc;
          if (!entry.is_regular_file(typeEc) || typeEc || !DirectoryScanner::isImagePath(entry.path())) {
            return;
          }
          WallpaperEntry e;
          e.name = entry.path().filename().string();
          e.absPath = entry.path();
          e.isDir = false;
          cacheMtime(entry, e);
          out.push_back(std::move(e));
        }
    );
  }

  void collectShallow(const std::filesystem::path& dir, std::vector<WallpaperEntry>& out) {
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
      if (ec) {
        break;
      }
      if (entry.path().filename().string().starts_with('.')) {
        continue;
      }

      std::error_code typeEc;
      if (entry.is_directory(typeEc) && !typeEc) {
        WallpaperEntry e;
        e.name = entry.path().filename().string();
        e.absPath = entry.path();
        e.isDir = true;
        out.push_back(std::move(e));
        continue;
      }
      if (entry.is_regular_file(typeEc) && !typeEc && DirectoryScanner::isImagePath(entry.path())) {
        WallpaperEntry e;
        e.name = entry.path().filename().string();
        e.absPath = entry.path();
        e.isDir = false;
        cacheMtime(entry, e);
        out.push_back(std::move(e));
      }
    }
  }

  void sortEntries(std::vector<WallpaperEntry>& entries) {
    // Directories first, then files; natural case-insensitive name order.
    std::ranges::sort(entries, [](const WallpaperEntry& a, const WallpaperEntry& b) {
      if (a.isDir != b.isDir) {
        return a.isDir;
      }
      return StringUtils::naturalCaseInsensitiveLess(a.name, b.name);
    });
  }

  WallpaperScanResult scanDirectory(const std::string& dir, bool flatten, std::filesystem::file_time_type dirMtime) {
    WallpaperScanResult result;
    result.dir = dir;
    result.flatten = flatten;
    result.dirMtime = dirMtime;
    if (flatten) {
      collectFlat(dir, result.entries);
    } else {
      collectShallow(dir, result.entries);
    }
    sortEntries(result.entries);
    return result;
  }

} // namespace

bool WallpaperScanner::requestScan(const std::filesystem::path& dir, bool flatten) {
  CacheKey key{dir.string(), flatten};

  std::error_code ec;
  if (dir.empty() || !std::filesystem::is_directory(dir, ec) || ec) {
    // Cache an empty result so cached() reports "scanned, nothing there".
    WallpaperScanResult empty;
    empty.dir = key.dir;
    empty.flatten = flatten;
    m_cache.insert_or_assign(key, std::move(empty));
    m_pending.erase(key);
    return true;
  }

  const auto mtime = std::filesystem::last_write_time(dir, ec);
  if (ec) {
    return true;
  }

  if (const auto it = m_cache.find(key); it != m_cache.end() && it->second.dirMtime == mtime) {
    return true; // fresh
  }

  if (!m_pending.insert(key).second) {
    return false; // already queued/in flight
  }

  BlockingPool::instance().run<WallpaperScanResult>(
      m_alive,
      [dir = key.dir, flatten, mtime] {
        WallpaperScanResult result = scanDirectory(dir, flatten, mtime);
        kLog.debug("scanned {} ({}): {} entries", result.dir, result.flatten ? "flat" : "shallow", result.entries.size());
        return result;
      },
      [this](WallpaperScanResult result) { apply(std::move(result)); }
  );
  return false;
}

const WallpaperScanResult* WallpaperScanner::cached(const std::filesystem::path& dir, bool flatten) const {
  const auto it = m_cache.find(CacheKey{dir.string(), flatten});
  return it == m_cache.end() ? nullptr : &it->second;
}

bool WallpaperScanner::scanning(const std::filesystem::path& dir, bool flatten) const {
  return m_pending.contains(CacheKey{dir.string(), flatten});
}

void WallpaperScanner::invalidate() { m_cache.clear(); }

void WallpaperScanner::apply(WallpaperScanResult result) {
  CacheKey key{result.dir, result.flatten};
  m_pending.erase(key);
  m_cache.insert_or_assign(std::move(key), std::move(result));
  if (m_onComplete) {
    m_onComplete();
  }
}
