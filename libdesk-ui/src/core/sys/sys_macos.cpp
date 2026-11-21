// memset_s is declared only with this set before the first C header.
#define __STDC_WANT_LIB_EXT1__ 1

#include "core/sys/sys.h"

#include <cerrno>
#include <chrono>
#include <crt_externs.h>
#include <csignal>
#include <poll.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <climits>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <glib.h>
#include <mach-o/dyld.h>
#include <malloc/malloc.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <util.h>
#include <vector>
#include <thread>

namespace sys {

  namespace {
    std::int64_t toNs(const timespec& ts) { return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec; }

    bool setNonblockCloexec(int fd) {
      const int flags = ::fcntl(fd, F_GETFL);
      return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 && ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
    }

    bool setCloexec(int fd) { return ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0; }
  } // namespace

  void setCurrentThreadName(const char* name) { pthread_setname_np(name); }

  std::uint64_t currentThreadId() {
    std::uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return id;
  }

  void secureZero(void* data, std::size_t len) { memset_s(data, len, 0, len); }

  // CLOCK_MONOTONIC on macOS keeps counting during sleep, like CLOCK_BOOTTIME.
  double bootTimeMs() { return static_cast<double>(clock_gettime_nsec_np(CLOCK_MONOTONIC)) / 1.0e6; }

  double processStartBootMs() {
    kinfo_proc info{};
    std::size_t size = sizeof(info);
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    timeval now{};
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || gettimeofday(&now, nullptr) != 0) {
      return bootTimeMs();
    }
    const timeval& start = info.kp_proc.p_starttime;
    const double agoMs = static_cast<double>(now.tv_sec - start.tv_sec) * 1000.0
        + static_cast<double>(now.tv_usec - start.tv_usec) / 1000.0;
    return bootTimeMs() - agoMs;
  }

  void installCrashThreadStack() {}

  void recordCrashBreadcrumb(char /*level*/, const char* /*section*/, std::string_view /*message*/) noexcept {}

  // Apple's libc++ has no time-zone database; GLib reads the system one.
  std::optional<ZoneInfo> zoneAt(std::string_view tz, std::int64_t unixSeconds) {
    GTimeZone* zone = g_time_zone_new_identifier(std::string(tz).c_str());
    if (zone == nullptr) {
      return std::nullopt;
    }
    const int interval = g_time_zone_find_interval(zone, G_TIME_TYPE_UNIVERSAL, unixSeconds);
    std::optional<ZoneInfo> info;
    if (interval >= 0) {
      info = ZoneInfo{
          .offsetSeconds = g_time_zone_get_offset(zone, interval),
          .dst = g_time_zone_is_dst(zone, interval) != FALSE,
          .abbrev = g_time_zone_get_abbreviation(zone, interval),
      };
    }
    g_time_zone_unref(zone);
    return info;
  }

  void releaseFreeHeap() { malloc_zone_pressure_relief(nullptr, 0); }

  // Mesa driver threads, the only users, exist only on Linux.
  void shareSchedulingWithThreads(std::string_view) {}

  std::int64_t mtimeNs(const struct stat& st) { return toNs(st.st_mtimespec); }

  bool statWithBirth(int dirFd, const char* name, struct stat& out, std::int64_t& birthNs) {
    if (::fstatat(dirFd, name, &out, AT_SYMLINK_NOFOLLOW) != 0) {
      return false;
    }
    birthNs = toNs(out.st_birthtimespec);
    return true;
  }

  bool readDirectory(int fd, const std::function<void(std::string_view name, unsigned char type)>& entry) {
    // fdopendir takes the fd; the caller keeps and closes its own.
    const int own = ::dup(fd);
    DIR* dir = own >= 0 ? ::fdopendir(own) : nullptr;
    if (dir == nullptr) {
      if (own >= 0) {
        ::close(own);
      }
      return false;
    }
    errno = 0;
    while (const dirent* d = ::readdir(dir)) {
      const std::string_view name(d->d_name, d->d_namlen);
      if (name != "." && name != "..") {
        entry(name, d->d_type);
      }
      errno = 0;
    }
    const int error = errno;
    ::closedir(dir);
    errno = error;
    return error == 0;
  }

  WakeFd::WakeFd() {
    int fds[2];
    if (::pipe(fds) != 0) {
      return;
    }
    if (!setNonblockCloexec(fds[0]) || !setNonblockCloexec(fds[1])) {
      ::close(fds[0]);
      ::close(fds[1]);
      return;
    }
    m_read = fds[0];
    m_write = fds[1];
  }

  WakeFd::~WakeFd() {
    if (m_read >= 0) {
      ::close(m_read);
      ::close(m_write);
    }
  }

  // A full pipe already holds a pending wake.
  bool WakeFd::signal() const {
    const char one = 1;
    return ::write(m_write, &one, 1) == 1 || errno == EAGAIN;
  }

  void WakeFd::drain() const {
    char buffer[64];
    while (::read(m_read, buffer, sizeof(buffer)) > 0) {
    }
  }

  int socketCloexec(int domain, int type) {
    const int fd = ::socket(domain, type, 0);
    const int on = 1;
    if (fd >= 0 && (!setCloexec(fd) || ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on) != 0)) {
      ::close(fd);
      return -1;
    }
    return fd;
  }

  bool pipeCloexec(int fds[2]) {
    if (::pipe(fds) != 0) return false;
    if (setCloexec(fds[0]) && setCloexec(fds[1])) return true;
    ::close(fds[0]);
    ::close(fds[1]);
    return false;
  }

  long recvmsgCloexec(int fd, struct msghdr* msg, int flags) {
    const ssize_t n = ::recvmsg(fd, msg, flags);
    if (n < 0) return n;
    for (cmsghdr* c = CMSG_FIRSTHDR(msg); c != nullptr; c = CMSG_NXTHDR(msg, c)) {
      if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
      const auto* fds = reinterpret_cast<const int*>(CMSG_DATA(c));
      const std::size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      for (std::size_t i = 0; i < count; ++i) setCloexec(fds[i]);
    }
    return n;
  }

  // Sockets from socketCloexec() carry SO_NOSIGPIPE; this covers sockets made elsewhere.
  long sendNoSignal(int fd, const void* data, std::size_t size, int flags) {
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
    return ::send(fd, data, size, flags);
  }

  int forkPty(int* master, const struct winsize* size) {
    return ::forkpty(master, nullptr, nullptr, const_cast<struct winsize*>(size));
  }

  std::string executablePath() {
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) return {};
    char resolved[PATH_MAX];
    return ::realpath(path.c_str(), resolved) != nullptr ? std::string(resolved) : std::string(path.c_str());
  }

  bool spawnDetached(const std::string& program, const std::vector<std::string>& args) {
    std::vector<char*> argv{const_cast<char*>(program.c_str())};
    for (const std::string& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    for (const int fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
      posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd == STDIN_FILENO ? O_RDONLY : O_WRONLY, 0);
    }
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT);
    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, program.c_str(), &actions, &attr, argv.data(), *_NSGetEnviron());
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) return false;
    std::thread([pid] { ::waitpid(pid, nullptr, 0); }).detach();
    return true;
  }

  std::string userRuntimeDir() {
    char dir[PATH_MAX];
    const std::size_t n = ::confstr(_CS_DARWIN_USER_TEMP_DIR, dir, sizeof dir);
    std::string out = n > 0 && n <= sizeof dir ? std::string(dir) : std::string("/tmp");
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
  }

  std::string userPath() {
    static const std::string path = [] {
      const char* inherited = std::getenv("PATH");
      const std::string fallback = inherited != nullptr ? inherited : "/usr/bin:/bin:/usr/sbin:/sbin";
      const char* shell = std::getenv("SHELL");
      if (shell == nullptr || *shell == '\0') {
        const passwd* pw = ::getpwuid(::getuid());
        shell = pw != nullptr && pw->pw_shell != nullptr ? pw->pw_shell : "/bin/zsh";
      }
      int fds[2];
      if (!pipeCloexec(fds)) return fallback;
      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
      posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
      posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
      // rc files may print: the marker finds the PATH after their output.
      constexpr std::string_view kMarker = "_ANDROMEDA_PATH_";
      char* argv[] = {const_cast<char*>(shell), const_cast<char*>("-ilc"),
                      const_cast<char*>("printf '_ANDROMEDA_PATH_%s' \"$PATH\""), nullptr};
      pid_t pid = -1;
      const int rc = ::posix_spawn(&pid, shell, &actions, nullptr, argv, *_NSGetEnviron());
      posix_spawn_file_actions_destroy(&actions);
      ::close(fds[1]);
      if (rc != 0) {
        ::close(fds[0]);
        return fallback;
      }
      std::string out;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        pollfd pfd{fds[0], POLLIN, 0};
        if (left.count() <= 0 || ::poll(&pfd, 1, static_cast<int>(left.count())) == 0) {
          ::kill(pid, SIGKILL);
          break;
        }
        char buffer[4096];
        const ssize_t n = ::read(fds[0], buffer, sizeof buffer);
        if (n > 0) {
          out.append(buffer, static_cast<std::size_t>(n));
        } else if (n == 0 || errno != EINTR) {
          break;
        }
      }
      ::close(fds[0]);
      ::waitpid(pid, nullptr, 0);
      const std::size_t at = out.rfind(kMarker);
      const std::string found = at == std::string::npos ? std::string() : out.substr(at + kMarker.size());
      return found.empty() || found.find('\n') != std::string::npos ? fallback : found;
    }();
    return path;
  }

  void forEachSessionProcess(int sid, const std::function<void(int pid)>& fn) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_SESSION, sid};
    std::size_t size = 0;
    if (::sysctl(mib, 4, nullptr, &size, nullptr, 0) != 0) return;
    std::vector<kinfo_proc> procs(size / sizeof(kinfo_proc) + 8);
    size = procs.size() * sizeof(kinfo_proc);
    if (::sysctl(mib, 4, procs.data(), &size, nullptr, 0) != 0) return;
    for (std::size_t i = 0; i < size / sizeof(kinfo_proc); ++i) {
      const pid_t pid = procs[i].kp_proc.p_pid;
      if (pid > 0 && pid != sid) fn(pid);
    }
  }

} // namespace sys
