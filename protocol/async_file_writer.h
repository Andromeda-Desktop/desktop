#pragma once

#include <filesystem>
#include <functional>
#include <initializer_list>
#include <optional>
#include <cstdint>
#include <string>
#include <string_view>

namespace andromeda {

  // Writes `text` to `path`; false when the write failed.
  using WriteFn = std::function<bool(const std::filesystem::path& path, const std::string& text)>;
  // Gets the result of a write or file operation, on the main loop.
  using WriteDone = std::function<void(bool ok)>;
  // Runs a closure on the process's main loop; callable from any thread.
  using MainLoopPoster = std::function<void(std::function<void()>)>;

  // Set once per process before any write with a `done`; a write with a `done` and no poster is a programming error.
  void setFileWriteDonePoster(MainLoopPoster poster);

  // Replaces `path` whole on a background thread, so a slow disk never stalls the main thread: an empty `write`
  // keeps the default (temporary file + rename); a caller passes its own for permissions or symlinks. Writes keep
  // their order; a newer write to the same path replaces a pending one, with its `write`, and the replaced write's
  // `done` gets the newer write's result. Pending writes finish at exit. The default writer removes temporary files
  // that dead processes left in the directory, once per directory and process.
  void writeFileAsync(std::filesystem::path path, std::string text, WriteFn write = {}, WriteDone done = {});

  // The default write: a temporary file in the same directory, then rename. Missing directories get `dirMode`, the
  // file `fileMode` (both less the umask). A WriteFn for private files passes 0600 and 0700.
  bool writeFileReplacing(
      const std::filesystem::path& path, std::string_view text, unsigned fileMode = 0666, unsigned dirMode = 0777
  );
  // The same, for a file written from several buffers in order (a header and a payload) without joining them.
  bool writeFileReplacing(
      const std::filesystem::path& path, std::initializer_list<std::string_view> parts, unsigned fileMode = 0666,
      unsigned dirMode = 0777
  );

  // Writes `value` to a kernel attribute file (sysfs, cgroupfs) in place: no create, no truncate, no temporary file.
  // These files live in memory, so the write never waits on a disk.
  bool writeKernelAttribute(const std::filesystem::path& path, std::string_view value);

  // Marks the calling thread as a running main loop while it lives; a loop's run function holds one. A synchronous
  // disk write on a marked thread stalls every frame and event behind it.
  class LoopThreadScope {
  public:
    LoopThreadScope() noexcept;
    ~LoopThreadScope();
    LoopThreadScope(const LoopThreadScope&) = delete;
    LoopThreadScope& operator=(const LoopThreadScope&) = delete;
  };

  // Every synchronous write owner calls this before it writes `path`: a write on a running loop thread to a path
  // outside $XDG_RUNTIME_DIR (tmpfs) is logged once per path and counted.
  void noteBlockingWrite(const std::filesystem::path& path);
  // Writes noteBlockingWrite counted in this process.
  [[nodiscard]] std::uint64_t blockingLoopWrites() noexcept;

  // Runs `op` on the same background thread, after every write and operation posted before it; it is never replaced
  // or merged. For file work that is not one whole-file replace (a download streamed in pieces, a rename).
  void runFileOpAsync(std::function<bool()> op, WriteDone done = {});

  // The text a queued or running writeFileAsync will leave at `path`, or nullopt when none is pending: a reader that
  // must see its own write reads this before the disk.
  [[nodiscard]] std::optional<std::string> pendingFileText(const std::filesystem::path& path);

} // namespace andromeda
