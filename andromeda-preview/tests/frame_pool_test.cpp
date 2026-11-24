#include "../src/frame_pool.h"

#include <cstdio>
#include <map>
#include <random>
#include <set>

// Random present and release sequences against a model of which buffers the compositor holds.
int main() {
  struct FakeBuffer {
    std::uint64_t key = 0;
  };
  struct Owner {};
  using Pool = preview::FramePool<FakeBuffer, Owner>;
  int failures = 0;
  auto check = [&](bool ok, const char* what, unsigned seed, int step) {
    if (!ok) {
      std::fprintf(stderr, "FAIL seed %u step %d: %s\n", seed, step, what);
      ++failures;
    }
  };
  for (unsigned seed = 1; seed <= 300 && failures == 0; ++seed) {
    std::mt19937 rng(seed);
    Owner owner;
    Pool pool;
    std::set<Pool::Entry*> held;     // attached, not yet released by the compositor
    std::set<FakeBuffer*> live;      // made, not yet destroyed
    std::map<FakeBuffer*, std::uint64_t> lastUse;
    std::uint64_t uses = 0;
    const std::uint64_t ring = 1 + rng() % 12; // memory blocks the helper cycles through
    std::uint32_t nextFrame = 1;
    auto destroy = [&](FakeBuffer* buffer) {
      for (Pool::Entry* e : held) {
        check(e->buffer != buffer, "destroyed a buffer the compositor holds", seed, 0);
      }
      check(live.erase(buffer) == 1, "destroyed an unknown buffer", seed, 0);
      delete buffer;
    };
    for (int step = 1; step <= 400; ++step) {
      const bool present = held.empty() || rng() % 3 != 0;
      if (present) {
        // The helper sends memory the window released; an untrusted one sometimes resends a held block.
        const std::uint64_t key = rng() % ring;
        bool idleExists = false;
        for (FakeBuffer* b : live) {
          bool isHeld = false;
          for (Pool::Entry* e : held) {
            isHeld = isHeld || e->buffer == b;
          }
          idleExists = idleExists || (!isHeld && b->key == key);
        }
        const std::uint32_t frame = nextFrame++;
        Pool::Entry* entry = pool.reuse({key}, frame);
        check(entry != nullptr || !idleExists, "made a buffer while an idle one of the key exists", seed, step);
        if (entry == nullptr) {
          auto* buffer = new FakeBuffer{key};
          live.insert(buffer);
          entry = pool.add(&owner, buffer, {key}, frame);
        }
        check(!held.contains(entry), "attached a buffer the compositor still holds", seed, step);
        check(entry->buffer->key == key && entry->key == Pool::Key{key}, "buffer shows another key", seed, step);
        check(entry->frame == frame && entry->owner == &owner, "entry lost its frame or owner", seed, step);
        held.insert(entry);
        lastUse[entry->buffer] = ++uses;
        pool.trim(destroy);
      } else {
        auto it = held.begin();
        std::advance(it, rng() % held.size());
        Pool::Entry* entry = *it;
        const auto frame = Pool::release(*entry);
        check(frame.has_value(), "a release returned no frame", seed, step);
        check(!Pool::release(*entry).has_value(), "a second release returned a frame", seed, step);
        held.erase(it);
      }
      check(pool.size() == live.size(), "pool size differs from live buffers", seed, step);
      check(!present || pool.size() <= held.size() + Pool::kWindow, "idle buffers past the window", seed, step);
      for (FakeBuffer* b : live) {
        bool isHeld = false;
        for (Pool::Entry* e : held) {
          isHeld = isHeld || e->buffer == b;
        }
        // The pool trims on present; a release between presents only makes a buffer idle.
        check(!present || isHeld || lastUse[b] + Pool::kWindow > uses, "kept an idle buffer outside the window", seed,
              step);
      }
    }
    pool.clear([&](FakeBuffer* b) {
      live.erase(b);
      delete b;
    });
    check(live.empty(), "clear left buffers", seed, 0);
  }
  return failures == 0 ? 0 : 1;
}
