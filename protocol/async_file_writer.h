#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace andromeda {

  // Writes `text` to `path`; false when the write failed.
  using WriteFn = std::function<bool(const std::filesystem::path& path, const std::string& text)>;

  // Replaces `path` whole on a background thread, so a slow disk never stalls the main thread: an empty `write`
  // keeps the default (temporary file + rename); a caller passes its own for permissions or symlinks. Writes keep
  // their order; a newer write to the same path replaces a pending one, with its `write`. Pending writes finish at
  // exit.
  void writeFileAsync(std::filesystem::path path, std::string text, WriteFn write = {});

  // The text a queued or running writeFileAsync will leave at `path`, or nullopt when none is pending: a reader that
  // must see its own write reads this before the disk.
  [[nodiscard]] std::optional<std::string> pendingFileText(const std::filesystem::path& path);

} // namespace andromeda
