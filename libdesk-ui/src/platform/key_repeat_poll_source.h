#pragma once

#include "core/poll_source.h"
#include "platform/display.h"

class KeyRepeatPollSource final : public PollSource {
public:
  explicit KeyRepeatPollSource(Display& display) : m_display(display) {}

  [[nodiscard]] int pollTimeoutMs() const override { return m_display.repeatPollTimeoutMs(); }

  void dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) override { m_display.repeatTick(); }

protected:
  void doAddPollFds(std::vector<pollfd>& /*fds*/) override {}

private:
  Display& m_display;
};
