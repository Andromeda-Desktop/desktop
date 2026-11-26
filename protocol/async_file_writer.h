#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

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
      const std::filesystem::path& path, const std::string& text, unsigned fileMode = 0666, unsigned dirMode = 0777
  );

  // Runs `op` on the same background thread, after every write and operation posted before it; it is never replaced
  // or merged. For file work that is not one whole-file replace (a download streamed in pieces, a rename).
  void runFileOpAsync(std::function<bool()> op, WriteDone done = {});

  // The text a queued or running writeFileAsync will leave at `path`, or nullopt when none is pending: a reader that
  // must see its own write reads this before the disk.
  [[nodiscard]] std::optional<std::string> pendingFileText(const std::filesystem::path& path);

} // namespace andromeda
