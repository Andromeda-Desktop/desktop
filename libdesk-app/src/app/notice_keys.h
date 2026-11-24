#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>

// Keyed notifications against an async server: per key, the id on screen and at most one Notify out. A notify while
// one is out waits and goes with the reply's id, so an update replaces the notice instead of adding a second one. A
// close drops the waiting notice; a Notify still out when its key closed is closed on arrival.
class NoticeKeys {
public:
  struct Request {
    std::uint32_t replaces; // 0: none
  };
  struct Answer {
    bool keep;                    // false: close the id that answered
    std::optional<Request> next;  // send the waiting notice now
  };

  // nullopt: a Notify for the key is out; the notice waits for its answer.
  std::optional<Request> notify(const std::string& key) {
    Key& k = m_keys[key];
    switch (k.out) {
    case Out::None:
      k.out = Out::Sent;
      return Request{k.shown.value_or(0)};
    case Out::Sent:
    case Out::SentWaiting:
      k.out = Out::SentWaiting;
      return std::nullopt;
    case Out::Closed:
    case Out::ClosedWaiting:
      k.out = Out::ClosedWaiting;
      return std::nullopt;
    }
    return std::nullopt;
  }

  // The id to close now, if one is shown; a Notify still out is closed when it answers.
  std::optional<std::uint32_t> close(const std::string& key) {
    const auto it = m_keys.find(key);
    if (it == m_keys.end()) return std::nullopt;
    if (it->second.out != Out::None) it->second.out = Out::Closed;
    const auto shown = std::exchange(it->second.shown, std::nullopt);
    prune(it);
    return shown;
  }

  Answer replied(const std::string& key, std::uint32_t id) {
    const auto it = m_keys.find(key);
    if (it == m_keys.end()) return {.keep = false, .next = std::nullopt};
    Key& k = it->second;
    const bool keep = k.out == Out::Sent || k.out == Out::SentWaiting;
    if (keep) k.shown = id;
    return {.keep = keep, .next = settle(it)};
  }

  // The notice on screen before the failed Notify stays.
  std::optional<Request> failed(const std::string& key) {
    const auto it = m_keys.find(key);
    if (it == m_keys.end()) return std::nullopt;
    return settle(it);
  }

  void closedByServer(const std::string& key, std::uint32_t id) {
    const auto it = m_keys.find(key);
    if (it == m_keys.end() || it->second.shown != id) return;
    it->second.shown.reset();
    prune(it);
  }

  [[nodiscard]] bool empty() const { return m_keys.empty(); }

private:
  // Closed: close(key) came after the Notify out. Waiting: a newer notice waits for its answer.
  enum class Out : std::uint8_t { None, Sent, SentWaiting, Closed, ClosedWaiting };
  struct Key {
    std::optional<std::uint32_t> shown;
    Out out = Out::None;
  };

  std::optional<Request> settle(std::map<std::string, Key>::iterator it) {
    Key& k = it->second;
    if (k.out == Out::SentWaiting || k.out == Out::ClosedWaiting) {
      k.out = Out::Sent;
      return Request{k.shown.value_or(0)};
    }
    k.out = Out::None;
    prune(it);
    return std::nullopt;
  }

  void prune(std::map<std::string, Key>::iterator it) {
    if (!it->second.shown && it->second.out == Out::None) m_keys.erase(it);
  }

  std::map<std::string, Key> m_keys;
};
