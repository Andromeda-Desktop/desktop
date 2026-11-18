#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

struct stat;

// The OS layer: portable code reaches Linux- or macOS-only calls only through these (sys_linux.cpp, sys_macos.cpp).
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

  // The crash reporter: Linux writes its own report (protocols/crash_capture.h); macOS leaves it to ReportCrash.
  void installCrashThreadStack();
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

  [[nodiscard]] std::int64_t mtimeNs(const struct stat& st);

  // lstat of dirFd/name, plus the birth time in ns (the change time when the file system has none).
  [[nodiscard]] bool statWithBirth(int dirFd, const char* name, struct stat& out, std::int64_t& birthNs);

  // Calls entry(name, d_type) for each entry of the open directory fd except "." and ".."; false and errno on error.
  [[nodiscard]] bool readDirectory(int fd, const std::function<void(std::string_view name, unsigned char type)>& entry);

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
