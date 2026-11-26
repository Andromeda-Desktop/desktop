#include "core/log.h"
#include "protocols/async_file_writer.h"
#include "core/sys/sys.h"

namespace log_platform {

  void writerThreadStarted() {
    sys::installCrashThreadStack();
    // Named: the idle-thread checks count unnamed threads as on-demand workers that should have exited.
    sys::setCurrentThreadName("dsk-log");
  }

  void breadcrumb(char level, const char* section, std::string_view message) noexcept {
    sys::recordCrashBreadcrumb(level, section, message);
  }

} // namespace log_platform

namespace andromeda::file_writer_platform {

  void writerThreadStarted() {
    sys::installCrashThreadStack();
    sys::setCurrentThreadName("andro-filewrite");
  }

  bool inMemory(const std::filesystem::path& path) { return sys::inMemoryFilesystem(path); }

} // namespace andromeda::file_writer_platform
