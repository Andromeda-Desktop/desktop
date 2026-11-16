#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <variant>

// Kitty desktop notifications (OSC 99), which libghostty parses but never hands to the app.
// Supports title and body chunks (d, i, e, p=title|body) and the p=? capability query.
class Osc99Scanner {
public:
  struct Show {
    std::string title;
    std::string body;
  };
  struct Query {
    std::string id;
  };
  using Event = std::variant<Show, Query>;

  // Bytes may split a sequence anywhere; state carries over to the next call.
  void feed(std::span<const std::uint8_t> bytes, const std::function<void(Event)>& emit);

  // The reply to a Query, written back to the program.
  static std::string queryReply(const std::string& id);

private:
  enum class State { Ground, Escape, Prefix, Payload, PayloadEscape, Skip, SkipEscape };
  struct Pending {
    std::string title;
    std::string body;
  };

  void finish(const std::function<void(Event)>& emit);

  State m_state = State::Ground;
  std::size_t m_prefixAt = 0;
  std::string m_payload;
  std::map<std::string, Pending> m_pending;
};
