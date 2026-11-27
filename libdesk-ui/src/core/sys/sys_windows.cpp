#include "core/sys/sys.h"
#include "core/sys/poll.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <string>

namespace sys {

  int pollFds(pollfd* fds, nfds_t count, int timeoutMs) {
    std::array<HANDLE, MAXIMUM_WAIT_OBJECTS> handles{};
    DWORD n = 0;
    const auto waited = [&](const pollfd& entry) { return entry.fd >= 0 && (entry.events & POLLIN) != 0; };
    const auto indexOf = [&](HANDLE handle) {
      return static_cast<DWORD>(std::find(handles.begin(), handles.begin() + n, handle) - handles.begin());
    };
    for (nfds_t i = 0; i < count; ++i) {
      fds[i].revents = 0;
      if (!waited(fds[i])) {
        continue;
      }
      HANDLE handle = LongToHandle(fds[i].fd);
      if (indexOf(handle) < n) {
        continue;
      }
      if (n == handles.size()) {
        errno = EINVAL;
        return -1;
      }
      handles[n++] = handle;
    }
    const DWORD timeout = timeoutMs < 0 ? INFINITE : static_cast<DWORD>(timeoutMs);
    if (n == 0) {
      Sleep(timeout);
      return 0;
    }
    const DWORD first = WaitForMultipleObjects(n, handles.data(), FALSE, timeout);
    if (first == WAIT_TIMEOUT) {
      return 0;
    }
    if (first == WAIT_FAILED) {
      // As poll(2): a closed handle reports POLLNVAL on its own entries, and signaled ones still report POLLIN.
      int reported = 0;
      bool invalid = false;
      for (nfds_t i = 0; i < count; ++i) {
        if (!waited(fds[i])) {
          continue;
        }
        const DWORD state = WaitForSingleObject(LongToHandle(fds[i].fd), 0);
        invalid = invalid || state == WAIT_FAILED;
        fds[i].revents = state == WAIT_FAILED ? POLLNVAL
                         : state == WAIT_OBJECT_0 || state == WAIT_ABANDONED ? POLLIN
                                                                              : 0;
        reported += fds[i].revents != 0 ? 1 : 0;
      }
      if (!invalid) {
        errno = EINVAL;
        return -1;
      }
      return reported;
    }
    // The wait returns the lowest signaled index; the ones after it are checked without waiting.
    const DWORD firstIndex = first >= WAIT_ABANDONED_0 ? first - WAIT_ABANDONED_0 : first - WAIT_OBJECT_0;
    std::array<bool, MAXIMUM_WAIT_OBJECTS> ready{};
    ready[firstIndex] = true;
    for (DWORD i = firstIndex + 1; i < n; ++i) {
      const DWORD state = WaitForSingleObject(handles[i], 0);
      ready[i] = state == WAIT_OBJECT_0 || state == WAIT_ABANDONED;
    }
    int readyCount = 0;
    for (nfds_t i = 0; i < count; ++i) {
      if (waited(fds[i]) && ready[indexOf(LongToHandle(fds[i].fd))]) {
        fds[i].revents = POLLIN;
        ++readyCount;
      }
    }
    return readyCount;
  }

  WakeFd::WakeFd() {
    if (HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr); event != nullptr) {
      m_read = HandleToLong(event);
      m_write = m_read;
    }
  }

  WakeFd::~WakeFd() {
    if (m_read >= 0) {
      CloseHandle(LongToHandle(m_read));
    }
  }

  bool WakeFd::signal() const { return SetEvent(LongToHandle(m_write)) != 0; }

  void WakeFd::drain() const { ResetEvent(LongToHandle(m_read)); }

  void setCurrentThreadName(const char* name) {
    std::wstring wide;
    for (const char* c = name; *c != '\0'; ++c) {
      wide.push_back(static_cast<unsigned char>(*c));
    }
    SetThreadDescription(GetCurrentThread(), wide.c_str());
  }

  std::uint64_t currentThreadId() { return GetCurrentThreadId(); }

  // Windows Error Reporting writes the crash report.
  void installCrashThreadStack() {}

  void recordCrashBreadcrumb(char /*level*/, const char* /*section*/, std::string_view /*message*/) noexcept {}

  // Windows has no tmpfs; a RAM disk is rare enough to count as a disk.
  bool inMemoryFilesystem(const std::filesystem::path& /*path*/) { return false; }

} // namespace sys
