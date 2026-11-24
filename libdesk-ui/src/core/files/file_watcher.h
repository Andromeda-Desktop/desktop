#pragma once

#include "core/files/directory_watcher.h"

#include <cstdint>
#include <filesystem>
#include <functional>

// Watches single files through their parent directories on a DirectoryWatcher; a file need not exist yet.
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

  // Calls `callback` on the main loop at most once per loop turn, however many events arrive; an Overflow or Gone
  // of the directory also calls it. -1 when the directory cannot be watched.
  WatchId watch(const std::filesystem::path& filePath, Callback callback,
                WatchTrigger trigger = WatchTrigger::Modified);
  void unwatch(WatchId id) { m_dirs.unwatch(id); }

private:
  DirectoryWatcher& m_dirs;
};
