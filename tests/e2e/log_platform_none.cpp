// The log's platform hooks for E2E helper binaries built from a few sources: no crash handler, unnamed writer.
#include "core/log.h"

namespace log_platform {
  void writerThreadStarted() {}
  void breadcrumb(char, const char*, std::string_view) noexcept {}
} // namespace log_platform
