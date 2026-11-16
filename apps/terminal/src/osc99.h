#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace term {

  // A desktop notification a program asked for (OSC 9 / 777 / 99) or the terminal posts itself.
  struct Notice {
    enum class When : std::uint8_t { Always, Unfocused, Invisible };
    std::string title;
    std::string body;
    std::string key; // the program's OSC 99 id: a later notice with the same key replaces it; empty: none
    std::uint8_t urgency = 1; // 0 low, 1 normal, 2 critical
    When when = When::Always;
    bool focus = true;        // a click raises the session
    bool report = false;      // a click is reported to the program
    bool reportClose = false; // closing is reported to the program
    std::vector<std::string> buttons;
  };

  // Kitty desktop notifications (OSC 99), which libghostty parses but never hands to the app.
  // Handles chunks (i, d, e), p=title|body|buttons|close|?, u, o, a and c; icons and sounds are ignored.
  class Osc99Scanner {
  public:
    struct Show {
      Notice notice;
    };
    struct Close {
      std::string key;
    };
    struct Query {
      std::string key;
    };
    using Event = std::variant<Show, Close, Query>;

    // Bytes may split a sequence anywhere; state carries over to the next call.
    void feed(std::span<const std::uint8_t> bytes, const std::function<void(Event)>& emit);

    // Replies written back to the program.
    static std::string queryReply(const std::string& key);
    static std::string activatedReply(const std::string& key, int button); // button 0: the body
    static std::string closedReply(const std::string& key);

  private:
    enum class State : std::uint8_t { Ground, Escape, Prefix, Payload, PayloadEscape, Skip, SkipEscape };

    void finish(const std::function<void(Event)>& emit);

    State m_state = State::Ground;
    std::size_t m_prefixAt = 0;
    std::string m_payload;
    std::map<std::string, Notice> m_pending; // by key, until the chunk with d=1
  };

} // namespace term
