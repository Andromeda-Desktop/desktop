#pragma once

#include "core/poll_schedule.h"
#include "core/poll_source.h"

#include <optional>
#include <vector>
#include <poll.h>

// The event loop of a process without its own (shell MainLoop, compositor wl_event_loop): DeferredCall and
// TimerManager, then the added sources, through one PollSchedule. A windowless process calls run(); a loop with its
// own fds (AppLoop's display) calls runDeferred(), prepare(), its poll, dispatch().
class PollLoop {
public:
  // Main also runs DeferredCall and TimerManager, which belong to the main thread; Worker runs only the added sources,
  // for a loop on another thread.
  enum class Kind { Main, Worker };

  explicit PollLoop(Kind kind = Kind::Main) : m_kind(kind) {}
  PollLoop(const PollLoop&) = delete;
  PollLoop& operator=(const PollLoop&) = delete;

  void addSource(PollSource* source);
  void removeSource(PollSource* source);

  // Runs the queued deferred calls. False once quit() was called, before or during them.
  bool runDeferred();
  // Appends the sources' fds to `fds` (after any the caller polls itself); returns the poll timeout in ms, -1: none.
  int prepare(std::vector<pollfd>& fds);
  // Dispatches the sources of the last prepare() that `fds` (polled) made ready or whose deadline passed, until quit().
  void dispatch(const std::vector<pollfd>& fds);

  // Passes until quit(). Returns the quit code, 1 when poll fails.
  int run();
  void quit(int code = 0);
  // The quit code; 0 before quit().
  [[nodiscard]] int exitCode() const { return m_exitCode.value_or(0); }

private:
  Kind m_kind;
  std::vector<PollSource*> m_sources;
  PollSchedule m_schedule;
  std::optional<int> m_exitCode; // set by quit()
};
