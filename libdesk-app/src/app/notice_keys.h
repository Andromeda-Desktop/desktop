#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>

// Keyed notifications against an async server: per key, the id on screen and the newest Notify still out. Only that
// newest call may stay on screen; an older reply, or one whose key was closed while it was out, is closed on arrival.
class NoticeKeys {
public:
  struct Request {
    std::uint64_t gen;
    std::uint32_t replaces; // 0: none
  };
  enum class Reply : std::uint8_t { Keep, Close };

  Request notify(const std::string& key) {
    Key& k = m_keys[key];
    k.pending = Pending{.gen = ++m_gen, .closed = false};
    return {k.pending->gen, k.shown.value_or(0)};
  }

  // The id to close now, if one is shown; a Notify still out is closed when it answers.
  std::optional<std::uint32_t> close(const std::string& key) {
    const auto it = m_keys.find(key);
    if (it == m_keys.end()) return std::nullopt;
    if (it->second.pending) it->second.pending->closed = true;
    const auto shown = std::exchange(it->second.shown, std::nullopt);
    prune(it);
    return shown;
  }

  Reply replied(const std::string& key, std::uint64_t gen, std::uint32_t id) {
    const auto it = m_keys.find(key);
    if (it == m_keys.end()) return Reply::Close;
    const bool newest = it->second.pending && it->second.pending->gen == gen;
    if (newest && !it->second.pending->closed) {
      it->second.pending.reset();
      it->second.shown = id;
      return Reply::Keep;
    }
    if (newest) it->second.pending.reset();
    if (it->second.shown == id) it->second.shown.reset(); // it replaced the shown one, which goes with it
    prune(it);
    return Reply::Close;
  }

  void failed(const std::string& key, std::uint64_t gen) {
    const auto it = m_keys.find(key);
    if (it == m_keys.end() || !it->second.pending || it->second.pending->gen != gen) return;
    it->second.pending.reset();
    prune(it);
  }

  void closedByServer(const std::string& key, std::uint32_t id) {
    const auto it = m_keys.find(key);
    if (it == m_keys.end() || it->second.shown != id) return;
    it->second.shown.reset();
    prune(it);
  }

  [[nodiscard]] bool empty() const { return m_keys.empty(); }

private:
  struct Pending {
    std::uint64_t gen;
    bool closed; // close(key) came after this Notify was sent
  };
  struct Key {
    std::optional<std::uint32_t> shown;
    std::optional<Pending> pending;
  };
  void prune(std::map<std::string, Key>::iterator it) {
    if (!it->second.shown && !it->second.pending) m_keys.erase(it);
  }

  std::map<std::string, Key> m_keys;
  std::uint64_t m_gen = 0;
};

