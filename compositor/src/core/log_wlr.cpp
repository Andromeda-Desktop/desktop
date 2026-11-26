#include "core/log_wlr.h"

#include "core/log.h"
#include "protocols/async_file_writer.h"
#include "protocols/crash_capture.h"
#include "protocols/tmpfs.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <pthread.h>
#include <string_view>

namespace log_platform {

  void writerThreadStarted() {
    pthread_setname_np(pthread_self(), "andro-log");
    andromeda::crash::installThreadStack();
  }

  void breadcrumb(char level, const char* section, std::string_view message) noexcept {
    andromeda::crash::record(level, section, message);
  }

} // namespace log_platform

namespace andromeda::file_writer_platform {

  void writerThreadStarted() {
    pthread_setname_np(pthread_self(), "andro-filewrite");
    crash::installThreadStack();
  }

  bool inMemory(const std::filesystem::path& path) { return onTmpfs(path); }

} // namespace andromeda::file_writer_platform

void wlrLogHandler(enum wlr_log_importance importance, const char* fmt, va_list args) {
  // wlroots only applies the wlr_log_init verbosity in its default stderr logger; a custom callback receives every
  // message, WLR_DEBUG included.
  if (importance > wlr_log_get_verbosity()) {
    return;
  }
  LogLevel level = LogLevel::Debug;
  switch (importance) {
  case WLR_ERROR:
    level = LogLevel::Error;
    break;
  case WLR_INFO:
    level = LogLevel::Info;
    break;
  default:
    level = LogLevel::Debug;
    break;
  }

  // Truncation is fine: the logger caps line length anyway.
  std::array<char, 1024> buffer{};
  va_list copy;
  va_copy(copy, args);
  const int written = std::vsnprintf(buffer.data(), buffer.size(), fmt, copy);
  va_end(copy);
  if (written < 0) {
    return;
  }

  const auto length = std::min(static_cast<std::size_t>(written), buffer.size() - 1);
  const std::string_view message(buffer.data(), length);
  // EBUSY: xe completes an unused plane's previous commit just after the page-flip event, and libliftoff puts every
  // plane in each commit. Output::commitState retries it and logs a summary when a retry gives up.
  if (level == LogLevel::Error && message.ends_with("Atomic commit failed: Device or resource busy")) {
    level = LogLevel::Debug;
  }
  detail::logMessage(level, "wlr", message);
}
