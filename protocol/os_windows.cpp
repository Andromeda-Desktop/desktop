#include "os.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <limits>

namespace andromeda::os {

  namespace {

    bool writeAll(HANDLE file, std::string_view text) {
      while (!text.empty()) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(text.size(), std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (WriteFile(file, text.data(), chunk, &written, nullptr) == 0 || written == 0) {
          return false;
        }
        text.remove_prefix(written);
      }
      return true;
    }

  } // namespace

  long processId() { return static_cast<long>(GetCurrentProcessId()); }

  std::tm localTime(std::time_t time) {
    std::tm tm{};
    localtime_s(&tm, &time);
    return tm;
  }

  bool stderrWritable() { return true; }

  void writeStderr(std::string_view text) {
    if (HANDLE err = GetStdHandle(STD_ERROR_HANDLE); err != nullptr && err != INVALID_HANDLE_VALUE) {
      (void)writeAll(err, text);
    }
  }

  bool stderrIsJournal() { return false; }

  void redirectStdioIfTty(const std::filesystem::path& /*dir*/) {}

  std::thread startThreadSignalsBlocked(void (*fn)()) { return std::thread(fn); }

  void atFork(void (*/*prepare*/)(), void (*/*parent*/)(), void (*/*child*/)()) {}

  std::FILE* openAppend(const std::filesystem::path& path) { return _wfopen(path.c_str(), L"abN"); }

  bool makeDirectory(const std::filesystem::path& dir, unsigned /*mode*/) {
    return CreateDirectoryW(dir.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
  }

  bool writeAndReplace(const std::filesystem::path& temporary, const std::filesystem::path& path,
                       std::initializer_list<std::string_view> parts, unsigned /*fileMode*/) {
    // Share mode 0: removeIfUnheld in another process cannot delete it while it is open.
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      return false;
    }
    const bool written = std::ranges::all_of(parts, [file](std::string_view part) { return writeAll(file, part); });
    // Closed before the move, which needs the delete access share mode 0 denies; the fresh temporary is younger
    // than the sweep's dead age.
    CloseHandle(file);
    const bool ok = written &&
                    MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) {
      DeleteFileW(temporary.c_str());
    }
    return ok;
  }

  // DeleteFileW fails while a writer holds the file open without delete sharing.
  void removeIfUnheld(const std::filesystem::path& temporary) { DeleteFileW(temporary.c_str()); }

  bool writeExisting(const std::filesystem::path& path, std::string_view value) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      return false;
    }
    const bool ok = writeAll(file, value);
    CloseHandle(file);
    return ok;
  }

} // namespace andromeda::os
