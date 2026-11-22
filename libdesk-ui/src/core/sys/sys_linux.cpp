#include "core/sys/sys.h"

#include "protocols/crash_capture.h"

#include <cerrno>
#include <chrono>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <sstream>
#include <string>
#include <string.h>
#include <pty.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <cstddef>
#include <string_view>
#include <utility>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>
#include <thread>
#include <sys/wait.h>
#include <spawn.h>

extern char** environ;

namespace sys {

  void setCurrentThreadName(const char* name) { pthread_setname_np(pthread_self(), name); }

  std::uint64_t currentThreadId() { return static_cast<std::uint64_t>(::gettid()); }

  void secureZero(void* data, std::size_t len) { explicit_bzero(data, len); }

  double bootTimeMs() {
    timespec ts{};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1.0e6;
  }

  // Field 22 of /proc/self/stat: start time in clock ticks after boot.
  namespace {
    std::string g_notifySocket;
  } // namespace

  void takeServiceNotifySocket() {
    if (const char* socket = std::getenv("NOTIFY_SOCKET"); socket != nullptr) {
      g_notifySocket = socket;
      ::unsetenv("NOTIFY_SOCKET");
    }
  }

  void notifyServiceReady() {
    const std::string socketPath = std::exchange(g_notifySocket, {});
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socketPath.empty() || socketPath.size() >= sizeof(address.sun_path)
        || (socketPath.front() != '/' && socketPath.front() != '@')) {
      return;
    }
    std::memcpy(address.sun_path, socketPath.data(), socketPath.size());
    const bool abstract = socketPath.front() == '@';
    if (abstract) {
      address.sun_path[0] = '\0';
    }
    const auto length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + socketPath.size() + !abstract);
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return;
    }
    constexpr std::string_view kReady = "READY=1";
    while (::sendto(fd, kReady.data(), kReady.size(), MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&address), length) < 0
           && errno == EINTR) {
    }
    ::close(fd);
  }

  double processStartBootMs() {
    std::ifstream stat("/proc/self/stat");
    std::string line;
    std::getline(stat, line);
    const auto close = line.rfind(')');
    if (close == std::string::npos) {
      return bootTimeMs();
    }
    std::istringstream fields(line.substr(close + 2));
    std::string field;
    for (int i = 3; i <= 22 && fields >> field; ++i) {
    }
    const long ticksPerSecond = sysconf(_SC_CLK_TCK);
    return ticksPerSecond > 0 ? std::stod(field) * 1000.0 / static_cast<double>(ticksPerSecond) : bootTimeMs();
  }

  void installCrashThreadStack() { andromeda::crash::installThreadStack(); }

  void recordCrashBreadcrumb(char level, const char* section, std::string_view message) noexcept {
    andromeda::crash::record(level, section, message);
  }

  std::optional<ZoneInfo> zoneAt(std::string_view tz, std::int64_t unixSeconds) {
    try {
      const auto* zone = std::chrono::locate_zone(tz);
      if (zone == nullptr) {
        return std::nullopt;
      }
      const auto info = zone->get_info(std::chrono::sys_seconds{std::chrono::seconds{unixSeconds}});
      return ZoneInfo{
          .offsetSeconds = static_cast<std::int32_t>(info.offset.count()),
          .dst = info.save != std::chrono::minutes::zero(),
          .abbrev = info.abbrev,
      };
    } catch (const std::runtime_error&) {
      return std::nullopt;
    }
  }

  // glibc keeps freed pages until a trim.
  void releaseFreeHeap() { malloc_trim(0); }

  void shareSchedulingWithThreads(std::string_view namePart) {
    const int policy = sched_getscheduler(0) & ~SCHED_RESET_ON_FORK;
    sched_param param{};
    if ((policy != SCHED_RR && policy != SCHED_FIFO) || sched_getparam(0, &param) != 0) {
      return;
    }
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task", ec)) {
      std::ifstream commFile(entry.path() / "comm");
      std::string comm;
      std::getline(commFile, comm);
      if (comm.contains(namePart)) {
        sched_setscheduler(static_cast<pid_t>(std::stol(entry.path().filename().string())), policy | SCHED_RESET_ON_FORK,
                           &param);
      }
    }
  }

  std::int64_t mtimeNs(const struct stat& st) {
    return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
  }

  std::int64_t ctimeNs(const struct stat& st) {
    return static_cast<std::int64_t>(st.st_ctim.tv_sec) * 1'000'000'000 + st.st_ctim.tv_nsec;
  }

  // statx for the birth time; the same cost as fstatat.
  bool statWithBirth(int dirFd, const char* name, struct stat& out, std::int64_t& birthNs) {
    struct statx sx{};
    if (::statx(dirFd, name, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS | STATX_BTIME, &sx) != 0) {
      return false;
    }
    out = {};
    out.st_mode = sx.stx_mode;
    out.st_size = static_cast<off_t>(sx.stx_size);
    out.st_ino = sx.stx_ino;
    out.st_mtim = {.tv_sec = sx.stx_mtime.tv_sec, .tv_nsec = sx.stx_mtime.tv_nsec};
    const auto& birth = (sx.stx_mask & STATX_BTIME) != 0 ? sx.stx_btime : sx.stx_ctime;
    birthNs = static_cast<std::int64_t>(birth.tv_sec) * 1'000'000'000 + birth.tv_nsec;
    return true;
  }

  bool readDirectory(int fd, const std::function<void(std::string_view name, unsigned char type)>& entry) {
    constexpr std::size_t kDirentBuffer = 256 * 1024;
    std::vector<char> buffer(kDirentBuffer);
    while (true) {
      const long n = ::syscall(SYS_getdents64, fd, buffer.data(), buffer.size());
      if (n < 0) {
        return false;
      }
      if (n == 0) {
        return true;
      }
      // struct linux_dirent64: u64 ino, s64 off, u16 reclen, u8 type, then the NUL-terminated name.
      for (long at = 0; at < n;) {
        std::uint16_t reclen = 0;
        std::memcpy(&reclen, buffer.data() + at + 16, sizeof(reclen));
        const auto type = static_cast<unsigned char>(buffer[static_cast<std::size_t>(at) + 18]);
        const std::string_view name(buffer.data() + at + 19);
        at += reclen;
        if (name != "." && name != "..") {
          entry(name, type);
        }
      }
    }
  }

  WakeFd::WakeFd() {
    m_read = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    m_write = m_read;
  }

  WakeFd::~WakeFd() {
    if (m_read >= 0) {
      ::close(m_read);
    }
  }

  bool WakeFd::signal() const {
    const std::uint64_t one = 1;
    return ::write(m_write, &one, sizeof(one)) == static_cast<ssize_t>(sizeof(one)) || errno == EAGAIN;
  }

  void WakeFd::drain() const {
    std::uint64_t ignored = 0;
    while (::read(m_read, &ignored, sizeof(ignored)) > 0) {
    }
  }

  int socketCloexec(int domain, int type) { return ::socket(domain, type | SOCK_CLOEXEC, 0); }

  bool pipeCloexec(int fds[2]) { return ::pipe2(fds, O_CLOEXEC) == 0; }

  long recvmsgCloexec(int fd, struct msghdr* msg, int flags) { return ::recvmsg(fd, msg, flags | MSG_CMSG_CLOEXEC); }

  long sendNoSignal(int fd, const void* data, std::size_t size, int flags) { return ::send(fd, data, size, flags | MSG_NOSIGNAL); }

  int forkPty(int* master, const struct winsize* size) { return ::forkpty(master, nullptr, nullptr, size); }

  namespace {
    std::string g_executablePath;
  } // namespace

  void setExecutablePath(std::string path) { g_executablePath = std::move(path); }

  std::string executablePath() {
    if (!g_executablePath.empty()) {
      return g_executablePath;
    }
    std::error_code ec;
    std::string path = std::filesystem::read_symlink("/proc/self/exe", ec).string();
    return ec ? std::string() : path;
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
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, program.c_str(), &actions, &attr, argv.data(), environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) return false;
    std::thread([pid] { ::waitpid(pid, nullptr, 0); }).detach();
    return true;
  }

  std::string userRuntimeDir() {
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    return dir != nullptr ? std::string(dir) : std::string();
  }

  std::string userPath() {
    const char* path = std::getenv("PATH");
    return path != nullptr ? std::string(path) : std::string();
  }

  void forEachSessionProcess(int sid, const std::function<void(int pid)>& fn) {
    DIR* proc = ::opendir("/proc");
    if (proc == nullptr) return;
    while (const dirent* entry = ::readdir(proc)) {
      char* end = nullptr;
      const long pid = std::strtol(entry->d_name, &end, 10);
      if (*end != '\0' || pid <= 0 || pid == sid) continue;
      char path[32];
      std::snprintf(path, sizeof path, "/proc/%ld/stat", pid);
      const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
      if (fd < 0) continue;
      char stat[512];
      const ssize_t n = ::read(fd, stat, sizeof stat - 1);
      ::close(fd);
      if (n <= 0) continue;
      stat[n] = '\0';
      const char* afterName = std::strrchr(stat, ')'); // the name may hold spaces and parentheses
      int session = 0;
      if (afterName != nullptr && std::sscanf(afterName + 1, " %*c %*d %*d %d", &session) == 1 && session == sid) {
        fn(static_cast<int>(pid));
      }
    }
    ::closedir(proc);
  }

} // namespace sys
