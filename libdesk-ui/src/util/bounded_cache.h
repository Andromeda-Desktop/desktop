#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <limits>
#include <list>
#include <optional>
#include <unordered_map>
#include <utility>

// An LRU map with an entry cap and a byte cap; the one owner of bounded caches. Not thread-safe.
// The evict callback runs for every value that leaves the cache (cap, erase, clear, trim, idle), on the caller's
// thread. It gets the only copy of the key, mutable, so it can wipe it.
template <typename K, typename V, typename Hash = std::hash<K>>
class BoundedCache {
public:
  struct Limits {
    std::size_t entries = std::numeric_limits<std::size_t>::max();
    std::size_t bytes = std::numeric_limits<std::size_t>::max();
  };
  using SizeFn = std::function<std::size_t(const V&)>;
  using EvictFn = std::function<void(K&, V&)>;
  using Clock = std::chrono::steady_clock;

  BoundedCache(Limits limits, SizeFn size, EvictFn evict = {})
      : m_limits(limits), m_size(std::move(size)), m_evict(std::move(evict)) {}
  BoundedCache(const BoundedCache&) = delete;
  BoundedCache& operator=(const BoundedCache&) = delete;
  // Runs no evict callback: an owner with resources to release clears first.
  ~BoundedCache() = default;

  // Marks the entry most recently used.
  [[nodiscard]] V* find(const K& key) {
    const auto it = m_map.find(key);
    if (it == m_map.end()) {
      return nullptr;
    }
    m_order.splice(m_order.begin(), m_order, it->second.order);
    it->second.lastUsed = Clock::now();
    return &it->second.value;
  }

  [[nodiscard]] const V* peek(const K& key) const {
    const auto it = m_map.find(key);
    return it == m_map.end() ? nullptr : &it->second.value;
  }

  [[nodiscard]] bool contains(const K& key) const { return m_map.contains(key); }

  // Inserts or replaces, then evicts least recently used entries until both caps hold; the new entry always stays,
  // even when it alone is larger than the byte cap.
  V& put(K key, V value) {
    erase(key);
    const std::size_t bytes = m_size(value);
    auto [it, inserted] = m_map.try_emplace(std::move(key), Node{std::move(value), bytes, Clock::now(), {}});
    m_order.push_front(&it->first);
    it->second.order = m_order.begin();
    m_bytes += bytes;
    fit(m_limits, 1);
    return it->second.value;
  }

  // Measures the entry again after its value changed size in place.
  void remeasure(const K& key) {
    const auto it = m_map.find(key);
    if (it == m_map.end()) {
      return;
    }
    m_bytes -= it->second.bytes;
    it->second.bytes = m_size(it->second.value);
    m_bytes += it->second.bytes;
    fit(m_limits, 1);
  }

  // Removes the entry without the evict callback: the caller takes the value back.
  [[nodiscard]] std::optional<V> extract(const K& key) {
    const auto it = m_map.find(key);
    if (it == m_map.end()) {
      return std::nullopt;
    }
    m_bytes -= it->second.bytes;
    m_order.erase(it->second.order);
    return std::move(m_map.extract(it).mapped().value);
  }

  bool erase(const K& key) {
    const auto it = m_map.find(key);
    if (it == m_map.end()) {
      return false;
    }
    drop(it, m_evict);
    return true;
  }

  void clear() { fit({0, 0}, 0); }

  // Clears with `evict` in place of the cache's own callback (e.g. after a lost GL context: nothing to unload).
  void clear(const EvictFn& evict) {
    while (!m_order.empty()) {
      drop(m_map.find(*m_order.back()), evict);
    }
  }

  // Evicts entries not used since `cutoff`, oldest first, keeping the `keep` most recently used.
  void evictUnusedBefore(Clock::time_point cutoff, std::size_t keep = 1) {
    while (m_map.size() > keep) {
      const auto it = m_map.find(*m_order.back());
      if (it->second.lastUsed >= cutoff) {
        return;
      }
      drop(it, m_evict);
    }
  }

  // Evicts every entry that `pred(key, value)` selects.
  template <typename P> void eraseIf(P&& pred) {
    for (auto it = m_order.begin(); it != m_order.end();) {
      const auto node = m_map.find(**it);
      ++it;
      if (pred(node->first, std::as_const(node->second.value))) {
        drop(node, m_evict);
      }
    }
  }

  // Memory pressure: evicts down to `limits` without changing the caps.
  void trim(Limits limits) { fit(limits, 0); }

  void setLimits(Limits limits) {
    m_limits = limits;
    fit(m_limits, 0);
  }

  [[nodiscard]] std::size_t size() const noexcept { return m_map.size(); }
  [[nodiscard]] std::size_t bytes() const noexcept { return m_bytes; }
  [[nodiscard]] Limits limits() const noexcept { return m_limits; }

  // Most recently used first.
  template <typename F> void forEach(F&& fn) const {
    for (const K* key : m_order) {
      fn(*key, m_map.find(*key)->second.value);
    }
  }

private:
  using Order = std::list<const K*>;
  struct Node {
    V value;
    std::size_t bytes = 0;
    Clock::time_point lastUsed;
    typename Order::iterator order;
  };
  using Map = std::unordered_map<K, Node, Hash>;

  // Keeps at least `keep` most recently used entries.
  void fit(Limits limits, std::size_t keep) {
    while (m_map.size() > keep && (m_map.size() > limits.entries || m_bytes > limits.bytes)) {
      drop(m_map.find(*m_order.back()), m_evict);
    }
  }

  void drop(typename Map::iterator it, const EvictFn& evict) {
    m_bytes -= it->second.bytes;
    m_order.erase(it->second.order);
    auto node = m_map.extract(it);
    if (evict) {
      evict(node.key(), node.mapped().value);
    }
  }

  Limits m_limits;
  SizeFn m_size;
  EvictFn m_evict;
  Order m_order;
  Map m_map;
  std::size_t m_bytes = 0;
};
