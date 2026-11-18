// memset_s is declared only with this set before the first C header.
#define __STDC_WANT_LIB_EXT1__ 1

#include "core/sys/sys.h"

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <glib.h>
#include <malloc/malloc.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

namespace sys {

  namespace {
    std::int64_t toNs(const timespec& ts) { return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec; }

    bool setNonblockCloexec(int fd) {
      const int flags = ::fcntl(fd, F_GETFL);
      return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 && ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
    }
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

} // namespace sys
