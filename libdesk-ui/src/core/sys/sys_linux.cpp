#include "core/sys/sys.h"

#include "protocols/crash_capture.h"

#include <cerrno>
#include <chrono>
#include <stdexcept>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <malloc.h>
#include <pthread.h>
#include <sstream>
#include <string>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

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

  std::int64_t mtimeNs(const struct stat& st) {
    return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
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

} // namespace sys
