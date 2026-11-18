#pragma once

#include "core/poll_source.h"
#include "compositors/compositor_platform.h"

class EventStreamPollSource final : public PollSource {
public:
  explicit EventStreamPollSource(CompositorPlatform& platform) : m_platform(platform) {}

  [[nodiscard]] int pollTimeoutMs() const override { return m_platform.eventStreamPollTimeoutMs(); }

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override {
    m_platform.dispatchEventStreamPoll(fds, startIdx);
  }

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override { m_platform.addEventStreamPollFds(fds); }

private:
  CompositorPlatform& m_platform;
};
