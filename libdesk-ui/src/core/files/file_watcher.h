#pragma once

#include "core/files/directory_watcher.h"

#include <cstdint>
#include <filesystem>
#include <functional>

// Watches single files through their parent directories on a DirectoryWatcher; neither the file nor its directory
// need exist yet, and a directory deleted and made again is watched again.
class FileWatcher {
public:
  using WatchId = DirectoryWatcher::WatchId;
  using Callback = std::function<void()>;
  enum class WatchTrigger : std::uint8_t {
    Modified,       // any change to the name: created, written, moved or renamed onto it
    WriteCompleted, // the content is complete: a writer closed it, or it was moved or renamed onto the name
  };

  explicit FileWatcher(DirectoryWatcher& dirs) : m_dirs(dirs) {}

  FileWatcher(const FileWatcher&) = delete;
  FileWatcher& operator=(const FileWatcher&) = delete;

  // Calls `callback` on the main loop at most once per loop turn, however many events arrive; an Overflow, a Gone
  // of the directory and its return also call it. -1 when no ancestor of the file can be watched.
  WatchId watch(const std::filesystem::path& filePath, Callback callback,
                WatchTrigger trigger = WatchTrigger::Modified);
  void unwatch(WatchId id) { m_dirs.unwatch(id); }

private:
  DirectoryWatcher& m_dirs;
};
