// Random notify, close, reply, failure and server-close sequences through NoticeKeys and a fake notification server:
// once every reply is in, a key shows at most one notification, none after a close, and the newest after a notify.
#include "app/notice_keys.h"

#include <cstdio>
#include <deque>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <string>

namespace {

int failures = 0;
unsigned seed = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s (seed %u)\n", __FILE__, __LINE__, #c, seed); ++failures; } } while (0)

struct Call {
  std::string key;
  std::uint64_t gen;
  std::uint32_t replaces;
  int body;
};

struct Sim {
  NoticeKeys keys;
  std::map<std::uint32_t, std::pair<std::string, int>> shown; // server: id -> key, body
  std::deque<Call> calls;                                      // Notify calls out, answered in order
  std::map<std::string, std::optional<int>> want;              // key -> newest body, nullopt after close
  std::uint32_t nextId = 1;
  int nextBody = 1;
  std::mt19937 rng;

  explicit Sim(unsigned s) : rng(s) {}

  std::string pickKey() { return std::uniform_int_distribution<int>(0, 1)(rng) == 0 ? "a" : "b"; }

  void notify() {
    const std::string key = pickKey();
    const NoticeKeys::Request r = keys.notify(key);
    calls.push_back({key, r.gen, r.replaces, nextBody});
    want[key] = nextBody++;
  }

  void close() {
    const std::string key = pickKey();
    if (const auto id = keys.close(key)) shown.erase(*id);
    want[key] = std::nullopt;
  }

  void answer(bool fail) {
    if (calls.empty()) return;
    const Call c = calls.front();
    calls.pop_front();
    if (fail) {
      keys.failed(c.key, c.gen);
      if (want[c.key] == c.body) { // the newest notice never showed; the one it was to replace stays
        want[c.key] = std::nullopt;
        for (const auto& [id, notice] : shown) {
          if (notice.first == c.key) want[c.key] = notice.second;
        }
      }
      return;
    }
    const std::uint32_t id = c.replaces != 0 && shown.contains(c.replaces) ? c.replaces : nextId++;
    shown[id] = {c.key, c.body};
    if (keys.replied(c.key, c.gen, id) == NoticeKeys::Reply::Close) shown.erase(id);
  }

  // The user dismisses a notice on screen.
  void dismiss() {
    if (shown.empty()) return;
    auto it = shown.begin();
    std::advance(it, std::uniform_int_distribution<std::size_t>(0, shown.size() - 1)(rng));
    const auto [id, notice] = *it;
    shown.erase(it);
    keys.closedByServer(notice.first, id);
    if (want[notice.first] == notice.second) want[notice.first] = std::nullopt;
  }

  void step() {
    switch (std::uniform_int_distribution<int>(0, 9)(rng)) {
    case 0: case 1: case 2: notify(); break;
    case 3: close(); break;
    case 4: answer(true); break;
    case 5: dismiss(); break;
    default: answer(false); break;
    }
  }

  void settleAndCheck() {
    while (!calls.empty()) answer(false);
    for (const auto& [key, body] : want) {
      int count = 0;
      std::optional<int> on;
      for (const auto& [id, notice] : shown) {
        if (notice.first == key) {
          ++count;
          on = notice.second;
        }
      }
      CHECK(count <= 1);
      CHECK(on == body);
    }
  }
};

} // namespace

int main() {
  for (seed = 1; seed <= 500; ++seed) {
    Sim sim(seed);
    for (int i = 0; i < 200; ++i) {
      sim.step();
      if (i % 20 == 19) sim.settleAndCheck();
    }
    sim.settleAndCheck();
    for (const auto& [id, notice] : std::map(sim.shown)) sim.keys.close(notice.first);
    CHECK(sim.keys.empty());
  }
  if (failures > 0) return 1;
  std::puts("notice keys: ok");
  return 0;
}
