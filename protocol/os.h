#pragma once

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <initializer_list>
#include <string_view>
#include <thread>

// The OS calls of the shared log and file writer (log.cpp, async_file_writer.cpp): os_posix.cpp, os_windows.cpp.
namespace andromeda::os {

  [[nodiscard]] long processId();
  [[nodiscard]] std::tm localTime(std::time_t time);

  // stderr takes a write now without blocking. Windows: always true, so a write blocks while a pipe on stderr is full.
  [[nodiscard]] bool stderrWritable();
  void writeStderr(std::string_view text);
  // stderr is the systemd journal stream ($JOURNAL_STREAM names its device and inode).
  [[nodiscard]] bool stderrIsJournal();
  // A TTY on stdout or stderr is pointed at dir/andromeda-stderr.log, or at the null device when dir is empty or
  // cannot hold it. Windows: nothing; a GUI process has no console.
  void redirectStdioIfTty(const std::filesystem::path& dir);
  // Starts fn on a thread with every signal blocked, so the main thread's signal handling keeps them.
  [[nodiscard]] std::thread startThreadSignalsBlocked(void (*fn)());
  // Runs the callbacks around fork(); Windows has no fork.
  void atFork(void (*prepare)(), void (*parent)(), void (*child)());

  // Binary, not inherited by child processes.
  [[nodiscard]] std::FILE* openAppend(const std::filesystem::path& path);
  // One directory; true when it exists afterwards. Windows ignores mode: the parent's ACL applies.
  [[nodiscard]] bool makeDirectory(const std::filesystem::path& dir, unsigned mode);
  // Writes parts to temporary, held exclusively while open, then replaces path with it; removes it on failure.
  // POSIX: rename(2) is atomic. Windows: MoveFileExW, for which Microsoft states no atomicity; it fails while another
  // handle holds path open without FILE_SHARE_DELETE, so readers of these files open with it.
  [[nodiscard]] bool writeAndReplace(const std::filesystem::path& temporary, const std::filesystem::path& path,
                                     std::initializer_list<std::string_view> parts, unsigned fileMode);
  // Removes a temporary that no writer holds.
  void removeIfUnheld(const std::filesystem::path& temporary);
  // Writes value to an existing file (a kernel attribute), never creating it.
  [[nodiscard]] bool writeExisting(const std::filesystem::path& path, std::string_view value);

} // namespace andromeda::os
