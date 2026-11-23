#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// The one owner of pipe reads for Wayland data transfers (clipboard, primary
// selection, drag and drop).
namespace pipe_io {
enum class ReadError : std::uint8_t { None, TimedOut, TooLarge, Failed };

// A failed read carries no bytes: a partial transfer is worse than none.
struct ReadResult {
  std::string bytes;
  ReadError error = ReadError::None;
  [[nodiscard]] bool ok() const { return error == ReadError::None; }
};

constexpr std::chrono::milliseconds kReadDeadline{5000};

// Reads `fd` to its end and closes it, blocking. The whole read must end within
// `deadline` and stay within `maxBytes`.
ReadResult readPipe(int fd, std::size_t maxBytes = static_cast<std::size_t>(-1),
                    std::chrono::milliseconds deadline = kReadDeadline);
// readPipe off the main thread; `done` runs on the main loop, unless `alive`
// expired first.
void readPipeAsync(int fd, std::size_t maxBytes, std::weak_ptr<void> alive,
                   std::function<void(ReadResult)> done);
// Writes all of `bytes` to `fd` (does not close it).
void writeAll(int fd, const std::string &bytes);
} // namespace pipe_io
