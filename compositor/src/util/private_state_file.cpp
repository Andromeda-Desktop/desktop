#include "util/private_state_file.h"

#include "protocols/async_file_writer.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace andromeda::private_state {

  namespace {
    // Creates the parent directory and returns it opened, owned by us and set to 0700; -1 otherwise.
    int openPrivateDirectory(const std::filesystem::path& file) {
      if (file.empty() || !file.is_absolute()) {
        return -1;
      }
      std::error_code error;
      std::filesystem::create_directories(file.parent_path(), error);
      if (error) {
        return -1;
      }
      const int directory = open(file.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (directory < 0) {
        return -1;
      }
      struct stat info{};
      if (fstat(directory, &info) != 0 || info.st_uid != geteuid() || fchmod(directory, 0700) != 0) {
        close(directory);
        return -1;
      }
      return directory;
    }

    bool privateRegular(const struct stat& info) {
      return S_ISREG(info.st_mode) && info.st_uid == geteuid() && (info.st_mode & 077) == 0;
    }

    bool writeAll(int fd, std::string_view bytes) {
      while (!bytes.empty()) {
        const ssize_t count = write(fd, bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count <= 0) {
          return false;
        }
        bytes.remove_prefix(static_cast<size_t>(count));
      }
      return true;
    }

    // Writes a temporary file, fsyncs it, renames it over `file`, then fsyncs the directory.
    bool writeAtomic(const std::filesystem::path& file, std::string_view bytes) {
      noteBlockingWrite(file);
      const int directory = openPrivateDirectory(file);
      if (directory < 0) {
        return false;
      }
      const auto suffix = randomHex(16);
      if (!suffix) {
        close(directory);
        return false;
      }
      const std::string temporary = "." + file.filename().string() + "." + *suffix;
      const int fd = openat(directory, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
      if (fd < 0) {
        close(directory);
        return false;
      }
      const bool written = writeAll(fd, bytes) && fsync(fd) == 0;
      const bool closed = close(fd) == 0;
      const bool replaced =
          written && closed && renameat(directory, temporary.c_str(), directory, file.filename().c_str()) == 0;
      if (!replaced) {
        unlinkat(directory, temporary.c_str(), 0);
      } else {
        // The rename already committed the new state; a caller must not keep the old state after it.
        fsync(directory);
      }
      close(directory);
      return replaced;
    }
  } // namespace

  Lock::Lock(const std::filesystem::path& file, std::string_view lockName, std::chrono::milliseconds wait) {
    const int directory = openPrivateDirectory(file);
    if (directory < 0) {
      return;
    }
    m_fd = openat(directory, std::string(lockName).c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    close(directory);
    struct stat info{};
    if (m_fd < 0 || fstat(m_fd, &info) != 0 || !privateRegular(info)) {
      if (m_fd >= 0) {
        close(m_fd);
      }
      m_fd = -1;
      return;
    }
    const auto deadline = std::chrono::steady_clock::now() + wait;
    while (flock(m_fd, LOCK_EX | LOCK_NB) != 0) {
      if (errno != EWOULDBLOCK || std::chrono::steady_clock::now() >= deadline) {
        close(m_fd);
        m_fd = -1;
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  Lock::~Lock() {
    if (m_fd >= 0) {
      close(m_fd);
    }
  }

  ReadResult read(const std::filesystem::path& file, size_t maxBytes) {
    if (file.empty() || !file.is_absolute()) {
      return Unsafe{};
    }
    const int fd = open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
      if (errno == ENOENT) {
        return Missing{};
      }
      return Unsafe{};
    }
    struct stat info{};
    if (fstat(fd, &info) != 0 || !privateRegular(info) || info.st_size < 0
        || static_cast<uint64_t>(info.st_size) > maxBytes) {
      close(fd);
      return Unsafe{};
    }
    std::string bytes(static_cast<size_t>(info.st_size), '\0');
    size_t offset = 0;
    while (offset < bytes.size()) {
      const ssize_t count = ::read(fd, bytes.data() + offset, bytes.size() - offset);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        close(fd);
        return Unsafe{};
      }
      offset += static_cast<size_t>(count);
    }
    close(fd);
    return bytes;
  }

  Saver::Saver(
      wl_event_loop* loop, std::filesystem::path file, std::string lockName, size_t maxBytes, const char* thread
  )
      : m_file(std::move(file)), m_lockName(std::move(lockName)), m_maxBytes(maxBytes), m_worker(loop, thread) {}

  void Saver::post(
      std::function<std::optional<std::string>(const ReadResult&)> merge, std::function<void(bool saved)> done
  ) {
    if (!m_worker.ready()) {
      done(false);
      return;
    }
    m_worker.post<bool>(
        [file = m_file, lockName = m_lockName, maxBytes = m_maxBytes, merge = std::move(merge)] {
          // A main-thread reader or another compositor instance holds the lock only briefly.
          const Lock lock(file, lockName, std::chrono::seconds(1));
          if (!lock) {
            return false;
          }
          const std::optional<std::string> bytes = merge(read(file, maxBytes));
          return !bytes || (bytes->size() <= maxBytes && writeAtomic(file, *bytes));
        },
        std::move(done)
    );
  }

  void Saver::then(std::function<void()> done) {
    if (!m_worker.ready()) {
      done();
      return;
    }
    m_worker.post<bool>([] { return true; }, [done = std::move(done)](bool) { done(); });
  }

  std::optional<std::string> randomHex(size_t bytes) {
    std::array<unsigned char, 32> data{};
    if (bytes > data.size()) {
      return std::nullopt;
    }
    size_t offset = 0;
    while (offset < bytes) {
      const ssize_t count = getrandom(data.data() + offset, bytes - offset, 0);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        return std::nullopt;
      }
      offset += static_cast<size_t>(count);
    }
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes * 2);
    for (size_t index = 0; index < bytes; ++index) {
      result += digits[data[index] >> 4];
      result += digits[data[index] & 15];
    }
    return result;
  }

} // namespace andromeda::private_state
