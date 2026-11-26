#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace andromeda {

  // Replaces `path` whole (temporary file + rename) on a background thread, so a slow disk never stalls the main
  // thread. Writes keep their order; a newer write to the same path replaces a pending one. Pending writes finish
  // at exit.
  void writeFileAsync(std::filesystem::path path, std::string text);

  // The text a queued or running writeFileAsync will leave at `path`, or nullopt when none is pending: a reader that
  // must see its own write reads this before the disk.
  [[nodiscard]] std::optional<std::string> pendingFileText(const std::filesystem::path& path);

} // namespace andromeda
