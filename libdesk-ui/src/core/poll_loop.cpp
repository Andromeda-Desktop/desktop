#include "core/poll_loop.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "core/timer_manager.h"

#include <algorithm>
#include <cerrno>

namespace {
  constexpr Logger kLog("poll-loop");

  class DeferredSource final : public PollSource {
  public:
    void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override {
      if (startIdx < fds.size() && (fds[startIdx].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
        DeferredCall::drainWakeFd();
      }
      for (auto& fn : DeferredCall::takePending()) {
        if (fn) {
          fn();
        }
      }
    }

  private:
    void doAddPollFds(std::vector<pollfd>& fds) override {
      if (const int fd = DeferredCall::wakeFd(); fd >= 0) {
        fds.push_back({.fd = fd, .events = POLLIN, .revents = 0});
      }
    }
  };

  class TimerSource final : public PollSource {
  public:
    [[nodiscard]] int pollTimeoutMs() const override { return TimerManager::instance().pollTimeoutMs(); }
    void dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) override {
      TimerManager::instance().tick();
    }

  private:
    void doAddPollFds(std::vector<pollfd>& /*fds*/) override {}
  };

  DeferredSource g_deferred;
  TimerSource g_timers;
} // namespace

void PollLoop::addSource(PollSource* source) {
  if (source != nullptr && !std::ranges::contains(m_sources, source)) {
    m_sources.push_back(source);
  }
}

void PollLoop::removeSource(PollSource* source) {
  std::erase(m_sources, source);
  m_schedule.forget(source);
}

bool PollLoop::runDeferred() {
  if (m_exitCode) {
    return false;
  }
  if (m_kind == Kind::Worker) {
    return true;
  }
  for (auto& fn : DeferredCall::takePending()) {
    if (fn) {
      fn();
    }
  }
  return !m_exitCode;
}

int PollLoop::prepare(std::vector<pollfd>& fds) {
  std::vector<PollSource*> all;
  if (m_kind == Kind::Main) {
    all = {&g_deferred, &g_timers};
  }
  all.insert(all.end(), m_sources.begin(), m_sources.end());
  return m_schedule.prepare(all, fds, PollSchedule::Clock::now());
}

void PollLoop::dispatch(const std::vector<pollfd>& fds) {
  m_schedule.dispatch(fds, PollSchedule::Clock::now(), [this, &fds](PollSource& source, std::size_t start, PollSchedule::Wake) {
    if (!m_exitCode) {
      source.dispatch(fds, start);
    }
  });
}

int PollLoop::run() {
  std::vector<pollfd> fds;
  while (runDeferred()) {
    fds.clear();
    const int timeoutMs = prepare(fds);
    if (::poll(fds.data(), fds.size(), timeoutMs) < 0) {
      if (errno == EINTR) {
        continue;
      }
      kLog.error("poll failed");
      return 1;
    }
    dispatch(fds);
  }
  return *m_exitCode;
}

void PollLoop::quit(int code) {
  m_exitCode = code;
}
