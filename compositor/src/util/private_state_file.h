#pragma once

#include "core/main_loop_worker.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

// Owner-only state files: 0700 directory, 0600 file, no symlinks, atomic replace.
namespace andromeda::private_state {

  // Exclusive flock on `<dir of file>/<lockName>`; false when it is unsafe, or still held after `wait` (never block
  // the main thread: wait only on a worker).
  class Lock {
  public:
    Lock(const std::filesystem::path& file, std::string_view lockName, std::chrono::milliseconds wait = {});
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

  // The new file text (nullopt leaves the file as it is) and the result for `done`.
  template <typename Result> struct Merge {
    std::optional<std::string> bytes;
    Result result;
  };

  // The one writer of a state file, on its own thread. Each save runs in post order: the flock, a read, `merge` on
  // what it read, then an atomic replace (temporary file, fsync, rename, directory fsync). `done` runs on the event
  // loop with the merge result, or nullopt when the lock or the write failed (at once, before save() returns, when the
  // worker has no event loop). Destruction still runs every queued save (a revoke reaches the disk at exit); their
  // results are dropped.
  class Saver {
  public:
    // `thread`: static storage, 15 bytes at most.
    Saver(wl_event_loop* loop, std::filesystem::path file, std::string lockName, size_t maxBytes, const char* thread);

    template <typename Result>
    void save(std::function<Merge<Result>(const ReadResult&)> merge, std::function<void(std::optional<Result>)> done) {
      // Written on the saver thread before its result is queued; read only in `done`.
      auto result = std::make_shared<std::optional<Result>>();
      post(
          [merge = std::move(merge), result](const ReadResult& file) {
            Merge<Result> merged = merge(file);
            result->emplace(std::move(merged.result));
            return std::move(merged.bytes);
          },
          [done = std::move(done), result](bool saved) { done(saved ? std::move(*result) : std::nullopt); }
      );
    }

    // Runs `done` on the event loop after every save posted before it; touches no file.
    void then(std::function<void()> done);

    [[nodiscard]] const std::filesystem::path& file() const { return m_file; }

  private:
    void post(std::function<std::optional<std::string>(const ReadResult&)> merge, std::function<void(bool saved)> done);

    std::filesystem::path m_file;
    std::string m_lockName;
    size_t m_maxBytes;
    // Last member: its destructor runs the queued saves while the fields above live.
    MainLoopWorker m_worker;
  };

  std::optional<std::string> randomHex(size_t bytes);

} // namespace andromeda::private_state
