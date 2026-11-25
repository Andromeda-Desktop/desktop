#include "app/app_loop.h"

#include "core/log.h"
#include "core/sys/sys.h"
#include "render/core/startup_metrics.h"
#include "platform/surface.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <exception>
#include <optional>
#include <thread>
#include <poll.h>

namespace {
  constexpr Logger kLog("app-loop");

} // namespace

AppLoop::AppLoop() : m_display(createDisplay()), m_keyRepeat(*m_display) {
  m_loop.addSource(&m_keyRepeat);
}

AppLoop::~AppLoop() = default;

bool AppLoop::initialize(bool sharedGl, const std::string& appId) {
  m_sharedGl = sharedGl;
  sys::ignoreSigpipe();
  if (!display().connect(std::nullopt)) {
    kLog.error("failed to connect to the Wayland compositor");
    return false;
  }
  // EGL init loads Mesa (~50 ms) on a worker while this thread sets up text and the app opens and builds its first
  // window. Mesa uses its own Wayland event queue, and no context is made current on the worker.
  // GL attach waits only for EGL; the program preload goes on after it, and a program create waits for the preload.
  auto ready = std::make_shared<std::promise<void>>();
  m_eglReady = ready->get_future();
  m_egl = std::async(std::launch::async, [this, sharedGl, appId, ready] {
    try {
      m_gl.initialize(display(), sharedGl);
    } catch (...) {
      ready->set_exception(std::current_exception());
      return;
    }
    m_gl.preloadPrograms(appId, [&ready] { ready->set_value(); });
  });
  try {
    m_render.initializeText();
  } catch (const std::exception& e) {
    kLog.error("text setup failed: {}", e.what());
    return false;
  }
  m_render.attachGlLater([this] {
    const auto waitStart = std::chrono::steady_clock::now();
    m_eglReady.get();
    startup_metrics::recordEglWait(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count()
    );
    m_render.attachGl(m_gl);
  });
  return true;
}

void AppLoop::addSource(PollSource* source) {
  m_loop.addSource(source);
}

void AppLoop::removeSource(PollSource* source) {
  m_loop.removeSource(source);
}

void AppLoop::quit(int code) {
  m_loop.quit(code);
}

void AppLoop::recoverGraphics() {
  m_render.prepareForGraphicsReset();
  m_gl.recreateRootContext();
  m_render.restoreAfterGraphicsReset(m_gl);
  m_render.finishGraphicsResetRecovery();
}

bool AppLoop::reconnect(std::chrono::milliseconds timeout) {
  if (m_egl.valid()) {
    m_egl.wait();
  }
  m_render.cleanup();
  m_gl.cleanup();
  display().disconnect();
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  // A socket-activated compositor accepts at once; a stale socket refuses until the new compositor binds it.
  // Past the deadline the app exits as before the reconnect; a terminal's shells stay in andromeda-ptyd.
  for (auto wait = std::chrono::milliseconds(50);; wait = std::min(wait * 2, std::chrono::milliseconds(1000))) {
    if (!display().serverPresent()) {
      kLog.info("the compositor socket is gone: the session ended");
      return false;
    }
    try {
      if (display().connect(deadline)) {
        break;
      }
    } catch (const std::exception& e) {
      kLog.info("reconnect: {}", e.what());
    }
    display().disconnect();
    if (std::chrono::steady_clock::now() + wait > deadline) {
      kLog.error("no compositor took over within {} ms", timeout.count());
      return false;
    }
    std::this_thread::sleep_for(wait);
  }
  // EGL setup talks to the compositor on its own event queue with no time limit, so a worker runs it. Windows are
  // closed and their state flushed: past the limit the process ends at once, as the worker cannot be stopped.
  auto egl = std::make_shared<std::promise<void>>();
  std::future<void> eglDone = egl->get_future();
  std::thread([this, egl] {
    try {
      m_gl.initialize(display(), m_sharedGl);
      egl->set_value();
    } catch (...) {
      egl->set_exception(std::current_exception());
    }
  }).detach();
  if (eglDone.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
    kLog.error("GL setup on the new compositor took over 5 s; exiting");
    std::_Exit(1);
  }
  try {
    eglDone.get();
    m_render.initializeText();
  } catch (const std::exception& e) {
    kLog.error("GL setup after reconnect failed: {}", e.what());
    return false;
  }
  m_render.attachGl(m_gl);
  kLog.info("reconnected to the compositor");
  return true;
}

int AppLoop::run() {
  Display& display = this->display();
  // nullopt: go on with the next pass (the disconnect handler reconnected).
  auto handle = [&](PumpResult result) -> std::optional<int> {
    if (result == PumpResult::Error) {
      return 1;
    }
    if (!m_onDisconnect || !m_onDisconnect()) {
      return 0;
    }
    return std::nullopt;
  };

  std::vector<pollfd> fds;
  while (m_loop.runDeferred()) {
    const PollPrep prep = display.preparePoll();
    if (prep.result != PumpResult::Ok) {
      if (const auto code = handle(prep.result)) {
        return *code;
      }
      continue;
    }

    fds.assign(1, {.fd = display.pollFd(), .events = prep.events, .revents = 0});
    int timeoutMs = m_loop.prepare(fds);
    if (Surface::hasPendingFrameWork() || Surface::hasPendingRenders()) {
      timeoutMs = 0;
    }
    // A full send buffer with a 0 ms source would spin; wait up to one 60 Hz frame for POLLOUT.
    if ((prep.events & POLLOUT) != 0 && timeoutMs >= 0 && timeoutMs < 16) {
      timeoutMs = 16;
    }

    if (display.wait(fds.data(), static_cast<nfds_t>(fds.size()), timeoutMs) < 0) {
      display.cancelPoll();
      if (errno == EINTR) {
        continue;
      }
      kLog.error("poll failed");
      return 1;
    }
    if (const PumpResult result = display.finishPoll(fds[0].revents); result != PumpResult::Ok) {
      if (const auto code = handle(result)) {
        return *code;
      }
      continue;
    }

    m_loop.dispatch(fds);

    Surface::drainPendingFrameWork();
    Surface::drainPendingRenders();
  }
  return m_loop.exitCode();
}
