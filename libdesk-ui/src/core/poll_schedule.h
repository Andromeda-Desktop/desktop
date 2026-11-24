#pragma once

#include "core/poll_source.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <span>
#include <vector>

// The deadlines and fd ranges of one poll loop (AppLoop, the shell MainLoop). A source's pollTimeoutMs() arms an
// absolute deadline on first sight, kept at the earliest request until the source dispatches; -1 drops it.
class PollSchedule {
public:
  using Clock = std::chrono::steady_clock;

  struct Wake {
    bool fd = false;
    bool timeout = false;
  };

  // Appends the fds of `sources` to `fds` and returns the poll timeout in ms (ceil; -1: no deadline). Drops the
  // deadlines of sources missing from `sources`. vote, if set, sees each source's pollTimeoutMs().
  int prepare(std::span<PollSource* const> sources, std::vector<pollfd>& fds, Clock::time_point now,
              const std::function<void(PollSource&, int timeoutMs)>& vote = {});

  // For each source of the last prepare(), in order, that has a ready fd or a deadline passed at `now`: clears its
  // deadline and calls run(source, its first fd index, wake). Skips a source destroyed or forgotten since prepare().
  void dispatch(const std::vector<pollfd>& fds, Clock::time_point now,
                const std::function<void(PollSource&, std::size_t start, Wake wake)>& run);

  // Drops a removed source's deadline and its place in the current pass.
  void forget(const PollSource* source);

private:
  struct Entry {
    PollSource* source;
    std::weak_ptr<const void> alive;
    std::size_t start;
    std::size_t count;
  };
  struct Deadline {
    Clock::time_point at;
    std::uint64_t pass;
  };

  std::vector<Entry> m_pass;
  // Keyed by liveness, not address: a new source at a freed source's address gets no stale deadline.
  std::map<std::weak_ptr<const void>, Deadline, std::owner_less<>> m_deadlines;
  std::uint64_t m_passId = 0;
};
