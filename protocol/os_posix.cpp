#include "os.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <format>
#include <poll.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace andromeda::os {

  namespace {

    bool writeAll(int fd, std::string_view text) {
      std::size_t done = 0;
      while (done < text.size()) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) {
          continue;
        }
        if (n <= 0) {
          return false;
        }
        done += static_cast<std::size_t>(n);
      }
      return true;
    }

  } // namespace

  long processId() { return ::getpid(); }

  std::tm localTime(std::time_t time) {
    std::tm tm{};
    localtime_r(&time, &tm);
    return tm;
  }

  // A child shares stderr's file description, so it cannot be set O_NONBLOCK.
  bool stderrWritable() {
    pollfd fd{.fd = STDERR_FILENO, .events = POLLOUT, .revents = 0};
    return ::poll(&fd, 1, 0) == 1 && (fd.revents & POLLOUT) != 0;
  }

  void writeStderr(std::string_view text) { (void)!::write(STDERR_FILENO, text.data(), text.size()); }

  bool stderrIsJournal() {
    const char* stream = std::getenv("JOURNAL_STREAM");
    struct stat st{};
    return stream != nullptr && fstat(STDERR_FILENO, &st) == 0
        && std::format("{}:{}", st.st_dev, st.st_ino) == stream;
  }

  void redirectStdioIfTty(const std::filesystem::path& dir) {
    const bool stdoutIsTty = isatty(STDOUT_FILENO) != 0;
    const bool stderrIsTty = isatty(STDERR_FILENO) != 0;
    if (!stdoutIsTty && !stderrIsTty) {
      return;
    }
    int fd = -1;
    if (!dir.empty()) {
      fd = ::open((dir / "andromeda-stderr.log").c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    }
    if (fd < 0) {
      fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    }
    if (fd < 0) {
      return;
    }
    // dup2 clears O_CLOEXEC on the target, so spawned children inherit the redirect instead of the TTY.
    if (stdoutIsTty) {
      (void)::dup2(fd, STDOUT_FILENO);
    }
    if (stderrIsTty) {
      (void)::dup2(fd, STDERR_FILENO);
    }
    ::close(fd);
  }

  // A thread inherits the mask: with signals unblocked it would take a signal's default action (main may not have
  // blocked SIGINT/SIGTERM for its signalfd yet) and kill the process.
  std::thread startThreadSignalsBlocked(void (*fn)()) {
    sigset_t all;
    sigset_t previous;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &previous);
    std::thread thread(fn);
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    return thread;
  }

  void atFork(void (*prepare)(), void (*parent)(), void (*child)()) { (void)pthread_atfork(prepare, parent, child); }

  std::FILE* openAppend(const std::filesystem::path& path) { return std::fopen(path.c_str(), "ae"); }

  bool makeDirectory(const std::filesystem::path& dir, unsigned mode) {
    return ::mkdir(dir.c_str(), static_cast<mode_t>(mode)) == 0 || errno == EEXIST;
  }

  bool writeAndReplace(const std::filesystem::path& temporary, const std::filesystem::path& path,
                       std::initializer_list<std::string_view> parts, unsigned fileMode) {
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                          static_cast<mode_t>(fileMode));
    if (fd < 0) {
      return false;
    }
    // Held until close: removeIfUnheld in another process leaves a locked temporary alone.
    ::flock(fd, LOCK_EX);
    // rename is atomic: a crash mid-write keeps the previous file.
    const bool ok = std::ranges::all_of(parts, [fd](std::string_view part) { return writeAll(fd, part); }) &&
                    ::rename(temporary.c_str(), path.c_str()) == 0;
    if (!ok) {
      ::unlink(temporary.c_str());
    }
    ::close(fd);
    return ok;
  }

  void removeIfUnheld(const std::filesystem::path& temporary) {
    const int fd = ::open(temporary.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
      return;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
    }
    ::close(fd);
  }

  bool writeExisting(const std::filesystem::path& path, std::string_view value) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
      return false;
    }
    const bool ok = writeAll(fd, value);
    ::close(fd);
    return ok;
  }

} // namespace andromeda::os
