#pragma once

#include "core/sys/poll.h"

#include <memory>
#include <vector>

class PollSource {
public:
  PollSource() = default;
  // A copy is a new source with its own liveness.
  PollSource(const PollSource& /*other*/) {}
  PollSource& operator=(const PollSource& /*other*/) { return *this; }
  virtual ~PollSource() = default;

  // Expires when this source is destroyed: PollSchedule skips a source an earlier dispatch of the same pass destroyed.
  [[nodiscard]] std::weak_ptr<const void> liveness() const { return m_alive; }

  // Append any fds this source wants polled. Returns the starting index.
  std::size_t addPollFds(std::vector<pollfd>& fds) {
    auto start = fds.size();
    doAddPollFds(fds);
    return start;
  }

  // Minimum timeout this source needs before its next dispatch, or -1 for none.
  // The loop treats it as an absolute deadline from when it is first advertised.
  [[nodiscard]] virtual int pollTimeoutMs() const { return -1; }

  // Called after poll() returns. startIdx is what addPollFds returned.
  virtual void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) = 0;

protected:
  virtual void doAddPollFds(std::vector<pollfd>& fds) = 0;

private:
  std::shared_ptr<const void> m_alive = std::make_shared<char>();
};
