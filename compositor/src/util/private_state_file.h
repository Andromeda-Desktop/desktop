#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

// Owner-only state files: 0700 directory, 0600 file, no symlinks, atomic replace.
namespace andromeda::private_state {

  // Exclusive non-blocking flock on `<dir of file>/<lockName>`; false when it is held or unsafe.
  class Lock {
  public:
    Lock(const std::filesystem::path& file, std::string_view lockName);
    ~Lock();
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    explicit operator bool() const { return m_fd >= 0; }

  private:
    int m_fd = -1;
  };

  struct Missing {};
  // Unreadable, not a regular file, not owned by us, group/other bits set, or over the size cap.
  struct Unsafe {};
  using ReadResult = std::variant<Missing, Unsafe, std::string>;

  ReadResult read(const std::filesystem::path& file, size_t maxBytes);

  // Writes a temporary file, fsyncs it, renames it over `file`, then fsyncs the directory.
  bool writeAtomic(const std::filesystem::path& file, std::string_view bytes);

  std::optional<std::string> randomHex(size_t bytes);

} // namespace andromeda::private_state
