#include "render/core/startup_metrics.h"
#include "core/sys/sys.h"

#include "core/log.h"
#include "core/scoped_timer.h"

#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace startup_metrics {
  namespace {

    constexpr Logger kLog("startup");

    double g_eglInitMs = 0.0;
    double g_programMs = 0.0;
    int g_programCount = 0;
    bool g_firstFrameLogged = false;

  } // namespace

  double sinceProcessStart() {
    static const double start = sys::processStartBootMs();
    return sys::bootTimeMs() - start;
  }

  void recordEglInit(double ms) { g_eglInitMs += ms; }

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
    kLog.info(
        "first frame at {:.0f} ms after process start; EGL init {:.1f} ms; {} GL programs built in {:.1f} ms",
        sinceProcessStart(), g_eglInitMs, g_programCount, g_programMs
    );
    return true;
  }

} // namespace startup_metrics
