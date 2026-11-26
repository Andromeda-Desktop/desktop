#pragma once

// Linux only: platform layers include it, portable code never does.
#include <filesystem>
#include <linux/magic.h>
#include <sys/statfs.h>

namespace andromeda {

  // The nearest existing directory of `path` is on tmpfs ($XDG_RUNTIME_DIR, /dev/shm): its writes never wait on a disk.
  [[nodiscard]] inline bool onTmpfs(const std::filesystem::path& path) {
    std::filesystem::path dir = path.parent_path();
    struct statfs fs{};
    while (::statfs(dir.c_str(), &fs) != 0 && dir.has_relative_path()) {
      dir = dir.parent_path();
    }
    return fs.f_type == TMPFS_MAGIC;
  }

} // namespace andromeda
