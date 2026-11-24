#include "core/poll_schedule.h"

#include <algorithm>
#include <limits>

int PollSchedule::prepare(std::span<PollSource* const> sources, std::vector<pollfd>& fds, Clock::time_point now,
                          const std::function<void(PollSource&, int)>& vote) {
  ++m_passId;
  m_pass.clear();
  m_pass.reserve(sources.size());
  int timeoutMs = -1;
  for (PollSource* source : sources) {
    const std::size_t start = source->addPollFds(fds);
    std::weak_ptr<const void> alive = source->liveness();
    m_pass.push_back({.source = source, .alive = alive, .start = start, .count = fds.size() - start});
    const int t = source->pollTimeoutMs();
    if (vote) {
      vote(*source, t);
    }
    if (t < 0) {
      m_deadlines.erase(alive);
      continue;
    }
    const Clock::time_point wanted = now + std::chrono::milliseconds(t);
    auto [it, inserted] = m_deadlines.try_emplace(std::move(alive), Deadline{.at = wanted, .pass = m_passId});
    it->second.at = std::min(it->second.at, wanted);
    it->second.pass = m_passId;
    // Round up so poll() sleeps until a sub-millisecond deadline has passed.
    const auto left = std::chrono::ceil<std::chrono::milliseconds>(it->second.at - now).count();
    const int leftMs = left < 0 ? 0 : static_cast<int>(std::min<std::int64_t>(left, std::numeric_limits<int>::max()));
    timeoutMs = timeoutMs < 0 ? leftMs : std::min(timeoutMs, leftMs);
  }
  std::erase_if(m_deadlines, [this](const auto& entry) { return entry.second.pass != m_passId; });
  return timeoutMs;
}

void PollSchedule::dispatch(const std::vector<pollfd>& fds, Clock::time_point now,
                            const std::function<void(PollSource&, std::size_t, Wake)>& run) {
  // Index loop: run() may forget() sources, which clears their entries but never resizes m_pass.
  for (std::size_t i = 0; i < m_pass.size(); ++i) {
    const Entry& entry = m_pass[i];
    if (entry.source == nullptr || entry.alive.expired()) {
      continue;
    }
    const auto first = fds.begin() + static_cast<std::ptrdiff_t>(entry.start);
    const Wake wake{
        .fd = std::any_of(first, first + static_cast<std::ptrdiff_t>(entry.count),
                          [](const pollfd& p) { return p.revents != 0; }),
        .timeout = [&] {
          const auto deadline = m_deadlines.find(entry.alive);
          return deadline != m_deadlines.end() && now >= deadline->second.at;
        }(),
    };
    if (!wake.fd && !wake.timeout) {
      continue;
    }
    m_deadlines.erase(entry.alive);
    run(*entry.source, entry.start, wake);
  }
}

// Every deadline belongs to a source of the last prepare(), so m_pass finds it without dereferencing `source`.
void PollSchedule::forget(const PollSource* source) {
  for (Entry& entry : m_pass) {
    if (entry.source == source) {
      m_deadlines.erase(entry.alive);
      entry.source = nullptr;
    }
  }
}
