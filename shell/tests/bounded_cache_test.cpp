#include "util/bounded_cache.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <print>
#include <random>
#include <string>
#include <vector>

// Model test: random operation sequences against a reference LRU list. Invariants after every step:
// both caps hold (except the one just-put entry may stand alone over the byte cap), the byte total matches,
// the order matches the reference, and every value leaves through exactly one evict callback or extract.
namespace {

  struct Value {
    int id = 0;
    std::size_t bytes = 0;
  };

  int runSequence(std::uint32_t seed) {
    std::mt19937 rng(seed);
    const std::size_t entryCap = 1 + rng() % 6;
    const std::size_t byteCap = 1 + rng() % 40;
    std::map<int, int> evicted; // value id -> evict count
    std::vector<std::pair<std::string, Value>> model; // most recently used first
    BoundedCache<std::string, Value> cache{
        {.entries = entryCap, .bytes = byteCap}, [](const Value& v) { return v.bytes; },
        [&](std::string& key, Value& v) {
          ++evicted[v.id];
          key.assign(key.size(), '\0');
        }
    };
    int nextId = 0;
    int live = 0;
    const auto fail = [&](int step, const char* what) {
      std::println(stderr, "bounded_cache_test: seed {} step {}: {}", seed, step, what);
      return 1;
    };
    const auto modelFit = [&](std::size_t keep) {
      const auto total = [&] {
        std::size_t sum = 0;
        for (const auto& [k, v] : model) {
          sum += v.bytes;
        }
        return sum;
      };
      while (model.size() > keep && (model.size() > entryCap || total() > byteCap)) {
        model.pop_back();
        --live;
      }
    };
    for (int step = 0; step < 300; ++step) {
      const std::string key = "k" + std::to_string(rng() % 8);
      const auto inModel = std::ranges::find(model, key, &std::pair<std::string, Value>::first);
      switch (rng() % 5) {
      case 0:
      case 1: {
        const Value v{nextId++, rng() % 20};
        if (inModel != model.end()) {
          model.erase(inModel);
          --live;
        }
        model.insert(model.begin(), {key, v});
        ++live;
        modelFit(1);
        if (cache.put(key, v).id != v.id) {
          return fail(step, "put returned another value");
        }
        break;
      }
      case 2: {
        Value* got = cache.find(key);
        if ((got != nullptr) != (inModel != model.end())) {
          return fail(step, "find disagrees with the model");
        }
        if (got != nullptr) {
          std::rotate(model.begin(), inModel, inModel + 1);
        }
        break;
      }
      case 3:
        if (cache.erase(key) != (inModel != model.end())) {
          return fail(step, "erase disagrees with the model");
        }
        if (inModel != model.end()) {
          model.erase(inModel);
          --live;
        }
        break;
      default: {
        const auto taken = cache.extract(key);
        if (taken.has_value() != (inModel != model.end())) {
          return fail(step, "extract disagrees with the model");
        }
        if (taken.has_value()) {
          evicted[taken->id] += 1;
          model.erase(inModel);
          --live;
        }
        break;
      }
      }
      std::vector<std::string> order;
      cache.forEach([&](const std::string& k, const Value&) { order.push_back(k); });
      std::size_t bytes = 0;
      std::vector<std::string> want;
      for (const auto& [k, v] : model) {
        want.push_back(k);
        bytes += v.bytes;
      }
      if (order != want) {
        return fail(step, "LRU order differs from the model");
      }
      if (cache.bytes() != bytes || cache.size() != model.size()) {
        return fail(step, "byte or entry total differs");
      }
      if (cache.size() > entryCap || (cache.bytes() > byteCap && cache.size() > 1)) {
        return fail(step, "a cap does not hold");
      }
      for (const auto& [id, count] : evicted) {
        if (count != 1) {
          return fail(step, "a value left twice");
        }
      }
      if (static_cast<int>(evicted.size()) + live != nextId) {
        return fail(step, "a value left without a callback");
      }
    }
    cache.clear();
    if (cache.size() != 0 || cache.bytes() != 0 || static_cast<int>(evicted.size()) != nextId) {
      return fail(300, "clear left values");
    }
    return 0;
  }

  int idleCase() {
    int evictions = 0;
    BoundedCache<int, int> cache{{}, [](const int&) { return std::size_t{1}; }, [&](int&, int&) { ++evictions; }};
    cache.put(1, 1);
    cache.put(2, 2);
    const auto cutoff = BoundedCache<int, int>::Clock::now() + std::chrono::seconds(1);
    cache.evictUnusedBefore(cutoff);
    if (cache.size() != 1 || !cache.contains(2) || evictions != 1) {
      std::println(stderr, "bounded_cache_test: idle expiry must keep only the most recent entry");
      return 1;
    }
    cache.clear([](int&, int&) {});
    if (cache.size() != 0 || evictions != 1) {
      std::println(stderr, "bounded_cache_test: clear(evict) must use the given callback");
      return 1;
    }
    return 0;
  }

} // namespace

int main() {
  for (std::uint32_t seed = 1; seed <= 500; ++seed) {
    if (runSequence(seed) != 0) {
      return 1;
    }
  }
  return idleCase();
}
