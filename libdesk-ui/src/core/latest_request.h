#pragma once

#include <cstdint>

// Latest request wins: a reply applies only while its ticket is current. begin() supersedes every earlier ticket;
// cancel() (a close, a target change) supersedes them without starting a new one.
class LatestRequest {
public:
  struct Ticket {
    std::uint64_t id = 0;
  };

  [[nodiscard]] Ticket begin() noexcept { return Ticket{++m_current}; }
  void cancel() noexcept { ++m_current; }
  // A default Ticket{} (id 0) is never current.
  [[nodiscard]] bool current(Ticket ticket) const noexcept { return ticket.id != 0 && ticket.id == m_current; }

private:
  std::uint64_t m_current = 0;
};

// Latest epoch wins: every request started in one epoch (ticket()) applies until advance() starts the next.
class RequestEpoch {
public:
  [[nodiscard]] LatestRequest::Ticket ticket() const noexcept { return LatestRequest::Ticket{m_current}; }
  void advance() noexcept { ++m_current; }
  [[nodiscard]] bool current(LatestRequest::Ticket ticket) const noexcept { return ticket.id == m_current; }

private:
  std::uint64_t m_current = 1;
};
