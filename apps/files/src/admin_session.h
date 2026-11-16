#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

// The Files side of one administrator tab: talks to `andromeda-fileops-admin serve` (one JSON request per line, one
// reply per line, in order). No IO here: the host writes the lines this asks for and feeds back what the helper
// prints and how it exits, so the protocol and the state are testable without root.
class AdminSession {
public:
  enum class Phase : std::uint8_t {
    Starting, // pkexec runs; the password dialog may show
    Ready,    // idle, requests go out at once
    Busy,     // one request is out; later ones wait in order
    Failed,   // the helper is gone or broke the protocol: every request fails at once
    Closed,   // the tab closed
  };
  using Reply = std::function<void(const nlohmann::json& reply)>; // {"ok":true,...} or {"ok":false,"error":...}

  AdminSession(std::function<void(const std::string& line)> write, std::function<void()> changed);

  // op: {"op":"list","path":...}; the reply always comes, exactly once (an error one when the session fails).
  void request(nlohmann::json op, Reply reply);
  void onLine(std::string_view line);
  // pkexec exits 126 when the password dialog was dismissed.
  void onExit(int status);
  void close();

  [[nodiscard]] Phase phase() const noexcept { return m_phase; }
  // Why the session failed; empty unless phase() is Failed.
  [[nodiscard]] const std::string& failure() const noexcept { return m_failure; }

private:
  struct Pending {
    nlohmann::json op;
    Reply reply;
  };
  struct InFlight {
    std::uint64_t id = 0;
    Reply reply;
  };
  bool transition(Phase to, std::string failure = {});
  void pump();
  void fail(std::string why);
  // Answers the request in flight and every waiting one with this error.
  void drain(const std::string& error);

  std::function<void(const std::string&)> m_write;
  std::function<void()> m_changed;
  Phase m_phase = Phase::Starting;
  std::string m_failure;
  std::deque<Pending> m_queue;
  std::optional<InFlight> m_inFlight; // set exactly while m_phase is Busy
  std::uint64_t m_nextId = 1;
};
