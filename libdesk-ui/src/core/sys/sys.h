#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <string_view>
#include <spawn.h>

struct msghdr;
struct stat;
struct winsize;

// The OS layer: portable code reaches Linux- or macOS-only calls only through these (sys_linux.cpp, sys_macos.cpp;
// sys_posix.cpp holds the calls both share).
namespace sys {

  // Names the calling thread; Linux keeps the first 15 bytes.
  void setCurrentThreadName(const char* name);
  [[nodiscard]] std::uint64_t currentThreadId();

  // Zeroes memory with stores the compiler cannot drop.
  void secureZero(void* data, std::size_t len);

  // Milliseconds since boot, sleep included.
  [[nodiscard]] double bootTimeMs();
  // When this process started, on the bootTimeMs() clock.
  [[nodiscard]] double processStartBootMs();

  // Takes NOTIFY_SOCKET out of the environment, so started children do not inherit it. Call before any fork.
  void takeServiceNotifySocket();
  // Sends READY=1 to the socket takeServiceNotifySocket() took, once; later calls and no socket do nothing.
  void notifyServiceReady();

  // The crash reporter: Linux writes its own report (protocols/crash_capture.h); macOS leaves it to ReportCrash.
  void installCrashThreadStack();
  // The nearest existing directory of `path` is on a memory file system (tmpfs).
  [[nodiscard]] bool inMemoryFilesystem(const std::filesystem::path& path);
  void recordCrashBreadcrumb(char level, const char* section, std::string_view message) noexcept;

  struct ZoneInfo {
    std::int32_t offsetSeconds = 0;
    bool dst = false;
    std::string abbrev;
  };
  // The IANA zone tz at unixSeconds; nullopt for an unknown zone.
  [[nodiscard]] std::optional<ZoneInfo> zoneAt(std::string_view tz, std::int64_t unixSeconds);

  // Hands free heap pages back to the OS.
  void releaseFreeHeap();

  // Gives this process's threads whose name contains namePart the calling thread's real-time policy.
  void shareSchedulingWithThreads(std::string_view namePart);

  [[nodiscard]] std::int64_t mtimeNs(const struct stat& st);
  [[nodiscard]] std::int64_t ctimeNs(const struct stat& st);

  // lstat of dirFd/name, plus the birth time in ns (the change time when the file system has none).
  [[nodiscard]] bool statWithBirth(int dirFd, const char* name, struct stat& out, std::int64_t& birthNs);

  // Calls entry(name, d_type) for each entry of the open directory fd except "." and ".."; false and errno on error.
  [[nodiscard]] bool readDirectory(int fd, const std::function<void(std::string_view name, unsigned char type)>& entry);

  // Close-on-exec is atomic on Linux; macOS sets it right after the call, so a concurrent fork can inherit the fd.
  [[nodiscard]] int socketCloexec(int domain, int type);
  [[nodiscard]] bool pipeCloexec(int fds[2]);
  // recvmsg() whose received SCM_RIGHTS fds are close-on-exec.
  [[nodiscard]] long recvmsgCloexec(int fd, struct msghdr* msg, int flags);
  // send() that returns EPIPE instead of raising SIGPIPE.
  [[nodiscard]] long sendNoSignal(int fd, const void* data, std::size_t size, int flags);

  // forkpty(): the child leads a new session with the pty as its controlling terminal.
  [[nodiscard]] int forkPty(int* master, const struct winsize* size);
  // A close-on-exec fd that polls readable (or hung up) once the caller's child pid has exited; -1 on failure.
  [[nodiscard]] int childExitFd(int pid);
  // The app's own executable; empty when the OS does not tell.
  [[nodiscard]] std::string executablePath();
  // An andromeda-appd child runs the app from a library: executablePath() returns this path, not the zygote's.
  void setExecutablePath(std::string path);
  // Starts program in a new session with stdio on /dev/null so it outlives the caller. A double fork makes it no
  // child of the caller, so nothing reaps it here. False when the exec fails.
  [[nodiscard]] bool spawnDetached(const std::string& program, const std::vector<std::string>& args);
  // posix_spawn in a new process group with default signals; stdio[i] becomes fd i. Returns the pid or -errno.
  [[nodiscard]] int spawnInGroup(const std::string& program, const std::vector<std::string>& argv,
                                 const std::vector<std::string>& env, std::array<int, 3> stdio);
  // {"/bin/sh", "-c", command, "sh", args...}: args become $1, $2, ...
  [[nodiscard]] std::vector<std::string> shellArgv(std::string command, std::vector<std::string> args = {});
  // Between fork and exec: every signal back to SIG_DFL and an empty mask, so the parent's ignored (SIGPIPE) and
  // blocked signals never reach the program. Async-signal-safe.
  void resetChildSignals();
  // The same through posix_spawn attributes; keeps the flags already set on attr.
  void resetChildSignals(posix_spawnattr_t* attr);
  // Once at process start: a write to a closed pipe or socket fails with EPIPE instead of ending the process.
  void ignoreSigpipe();
  // The user's private directory for sockets: XDG_RUNTIME_DIR on Linux (empty when unset), the per-user temp dir
  // on macOS. No trailing slash.
  [[nodiscard]] std::string userRuntimeDir();
  // The PATH of the user's shell. A macOS app started from Finder or the Dock gets only launchd's minimal PATH, so
  // macOS asks `$SHELL -ilc` once (up to 2 s, cached) and falls back to PATH. Linux returns PATH.
  [[nodiscard]] std::string userPath();
  // Calls fn for every process of session sid except its leader.
  void forEachSessionProcess(int sid, const std::function<void(int pid)>& fn);

  // Wakes a poll loop from any thread: an eventfd on Linux, a pipe elsewhere. Both ends are nonblocking.
  class WakeFd {
  public:
    WakeFd();
    ~WakeFd();
    WakeFd(const WakeFd&) = delete;
    WakeFd& operator=(const WakeFd&) = delete;

    [[nodiscard]] bool valid() const noexcept { return m_read >= 0; }
    [[nodiscard]] int pollFd() const noexcept { return m_read; }
    // False and errno when the wake could not be queued.
    bool signal() const;
    void drain() const;

  private:
    int m_read = -1;
    int m_write = -1;
  };

} // namespace sys
