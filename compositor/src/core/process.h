#pragma once

#include <string>
#include <string_view>

namespace andromeda {

  // Undo everything the compositor did to the process that a child must not inherit. `wl_event_loop_add_signal` blocks
  // SIGINT/SIGTERM process-wide via sigprocmask, and a blocked mask survives fork and exec. Restore the child defaults
  // before exec, alongside `restoreFileDescriptorLimit`.
  void resetChildSignalState();

  // Reap exited children from a SIGCHLD handler. Unlike SIG_IGN, a handler is not inherited across exec, so programs
  // the compositor's libraries launch start with the default disposition: Xwayland waits for its own xkbcomp child and
  // cannot start when SIGCHLD is ignored. wlroots tolerates its Xwayland launcher being reaped here.
  void installChildReaper();

  // Start a shell command without racing the global child reaper; its output goes to stderr. Returns the read end of a
  // completion pipe (-1 when the fork failed): it yields 'x' when the command succeeded, else EOF without a byte.
  [[nodiscard]] int startShellCommand(const std::string& command);
  // Reads the completion pipe of startShellCommand once it is readable and closes it.
  [[nodiscard]] bool shellCommandSucceeded(int completionFd);

  // Send READY=1 from the compositor process. An empty socket path is a no-op.
  [[nodiscard]] bool notifyServiceReady(std::string_view socketPath);
  // Send one sd_notify state line (for example WATCHDOG=1). An empty socket path is a no-op.
  [[nodiscard]] bool notifyService(std::string_view socketPath, std::string_view state);

  // Close every non-standard descriptor before a managed application child
  // hands control to systemd-run. Returns false when a complete close cannot
  // be guaranteed.
  [[nodiscard]] bool closeChildFileDescriptors();

  // Resolve a bare executable against PATH, or validate an explicit path. An
  // empty result means no executable was found.
  [[nodiscard]] std::string resolveExecutable(const char* name);

} // namespace andromeda
