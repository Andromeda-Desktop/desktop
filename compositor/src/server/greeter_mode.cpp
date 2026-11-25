#include "server/greeter_mode.h"

#include "core/log.h"
#include "core/process.h"

#include <cstdlib>
#include <sys/syscall.h>
#include <unistd.h>
#include <wayland-server-core.h>

namespace andromeda {

  namespace {
    constexpr Logger kLog("greeter");
    bool g_greeterMode = false;
  } // namespace

  bool greeterMode() { return g_greeterMode; }
  void setGreeterMode(bool enabled) { g_greeterMode = enabled; }

  GreeterChild::GreeterChild(wl_event_loop* loop, const char* command, std::function<void()> onExit)
      : m_onExit(std::move(onExit)) {
    m_pid = forkChild([command] { execShell(command); });
    if (m_pid < 0) {
      kLog.error("greeter: fork failed");
      return;
    }
    m_pidfd = static_cast<int>(syscall(SYS_pidfd_open, m_pid, 0));
    if (m_pidfd < 0) {
      kLog.error("greeter: pidfd_open failed; the session will not end with the greeter");
      return;
    }
    m_source = wl_event_loop_add_fd(loop, m_pidfd, WL_EVENT_READABLE, handleExit, this);
    kLog.info("greeter: started pid={} command='{}'", m_pid, command);
  }

  GreeterChild::~GreeterChild() {
    if (m_source != nullptr) {
      wl_event_source_remove(m_source);
    }
    if (m_pidfd >= 0) {
      close(m_pidfd);
    }
  }

  int GreeterChild::handleExit(int /*fd*/, uint32_t /*mask*/, void* data) {
    auto* self = static_cast<GreeterChild*>(data);
    wl_event_source_remove(self->m_source);
    self->m_source = nullptr;
    kLog.info("greeter: pid={} exited; ending the greeter session", self->m_pid);
    self->m_onExit();
    return 0;
  }

} // namespace andromeda
