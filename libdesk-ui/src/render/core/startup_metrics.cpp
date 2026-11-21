#include "render/core/startup_metrics.h"
#include "core/sys/sys.h"

#include "core/log.h"
#include "core/scoped_timer.h"
#include "render/core/shader_program.h"

#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace startup_metrics {
  namespace {

    constexpr Logger kLog("startup");

    double g_eglInitMs = 0.0;
    double g_eglWaitMs = 0.0;
    int g_preloadedCount = 0;
    double g_programMs = 0.0;
    int g_programCount = 0;
    bool g_firstFrameLogged = false;

  } // namespace

  double sinceProcessStart() {
    static const double start = sys::processStartBootMs();
    return sys::bootTimeMs() - start;
  }

  void recordEglInit(double ms) { g_eglInitMs += ms; }

  void recordEglWait(double ms) { g_eglWaitMs += ms; }

  void recordProgramPreloaded() { ++g_preloadedCount; }

  void recordProgramBuild(double ms) {
    g_programMs += ms;
    ++g_programCount;
    if (g_firstFrameLogged && noctalia::profiling::enabled()) {
      kLog.info("GL program built after the first frame in {:.1f} ms", ms);
    }
  }

  bool recordFirstFrame() {
    if (g_firstFrameLogged) {
      return false;
    }
    g_firstFrameLogged = true;
    sys::notifyServiceReady();
    ShaderProgram::saveUsedForPreload();
    kLog.info(
        "first frame at {:.0f} ms after process start; EGL init {:.1f} ms; {} GL programs built in {:.1f} ms; "
        "{} preloaded; EGL wait {:.1f} ms",
        sinceProcessStart(), g_eglInitMs, g_programCount, g_programMs, g_preloadedCount, g_eglWaitMs
    );
    return true;
  }
  void recordSurfaceFirstFrame(std::string_view surface) {
    if (noctalia::profiling::enabled()) {
      kLog.info("surface {} first frame at {:.0f} ms after process start", surface, sinceProcessStart());
    }
  }


} // namespace startup_metrics
