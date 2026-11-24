#include "core/poll_schedule.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <new>
#include <optional>
#include <random>
#include <vector>

// Model test: random sequences of add, forget, destroy (also during dispatch, also into a freed address),
// timeout changes and fd wakes. Every pass checks the poll timeout and the exact dispatch sequence against a model.
namespace {
  using Clock = PollSchedule::Clock;
  int g_failures = 0;

  void check(bool ok, const char* what, int step) {
    if (!ok && ++g_failures <= 20) {
      std::fprintf(stderr, "FAIL step %d: %s\n", step, what);
    }
  }

  struct Fake final : PollSource {
    int id = 0;
    int timeoutMs = -1;
    int fds = 0;
    void dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) override {}
    [[nodiscard]] int pollTimeoutMs() const override { return timeoutMs; }

  protected:
    void doAddPollFds(std::vector<pollfd>& out) override {
      for (int i = 0; i < fds; ++i) {
        out.push_back({.fd = id * 10 + i, .events = POLLIN, .revents = 0});
      }
    }
  };

  constexpr std::size_t kSlots = 6;

  // Slots reuse their storage, so a new source often has a destroyed source's address.
  struct Slot {
    alignas(Fake) std::array<unsigned char, sizeof(Fake)> storage{};
    // The model: the id of the source in the slot, and its deadline.
    int id = 0;
    bool live = false;
    bool inLoop = false;
    std::optional<Clock::time_point> deadline;
    Fake* get() { return std::launder(reinterpret_cast<Fake*>(storage.data())); }
  };

  enum class Action { None, Destroy, DestroyAndForget, Forget, Create };
  struct Plan {
    Action action = Action::None;
    std::size_t target = 0;
  };
  struct Run {
    int id;
    bool fd;
    bool timeout;
    bool operator==(const Run&) const = default;
  };
} // namespace

int main() {
  std::mt19937 rng(7);
  auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
  std::array<Slot, kSlots> slots;
  PollSchedule schedule;
  Clock::time_point now{};
  int nextId = 1;

  auto create = [&](std::size_t s) {
    Fake* f = new (slots[s].storage.data()) Fake();
    f->id = nextId++;
    f->timeoutMs = pick(4) == 0 ? -1 : pick(40);
    f->fds = pick(3);
    slots[s].id = f->id;
    slots[s].live = true;
    slots[s].inLoop = true;
    slots[s].deadline.reset();
  };
  auto destroy = [&](std::size_t s) {
    slots[s].get()->~Fake();
    slots[s].live = false;
    slots[s].inLoop = false;
    slots[s].deadline.reset();
  };
  auto forget = [&](std::size_t s) {
    schedule.forget(slots[s].get());
    slots[s].inLoop = false;
    slots[s].deadline.reset();
  };

  for (int step = 0; step < 200000; ++step) {
    for (std::size_t s = 0; s < kSlots; ++s) {
      if (!slots[s].live && pick(3) == 0) {
        create(s);
      } else if (slots[s].live && !slots[s].inLoop && pick(4) == 0) {
        slots[s].inLoop = true;
      } else if (slots[s].live && pick(5) == 0) {
        slots[s].get()->timeoutMs = pick(4) == 0 ? -1 : pick(40);
      }
    }
    now += std::chrono::microseconds(pick(9000));

    // The loop lists its sources in any order and may leave one out for a pass.
    std::vector<std::size_t> order;
    for (std::size_t s = 0; s < kSlots; ++s) {
      if (slots[s].inLoop && pick(10) != 0) {
        order.push_back(s);
      }
    }
    std::shuffle(order.begin(), order.end(), rng);
    std::vector<PollSource*> list;
    std::vector<int> passIds;
    int expectTimeout = -1;
    for (std::size_t s = 0; s < kSlots; ++s) {
      if (std::find(order.begin(), order.end(), s) == order.end()) {
        slots[s].deadline.reset();
      }
    }
    for (std::size_t s : order) {
      Fake* f = slots[s].get();
      list.push_back(f);
      passIds.push_back(f->id);
      if (f->timeoutMs < 0) {
        slots[s].deadline.reset();
        continue;
      }
      const Clock::time_point wanted = now + std::chrono::milliseconds(f->timeoutMs);
      slots[s].deadline = slots[s].deadline ? std::min(*slots[s].deadline, wanted) : wanted;
      const auto left = std::chrono::ceil<std::chrono::milliseconds>(*slots[s].deadline - now).count();
      const int leftMs = static_cast<int>(std::max<std::int64_t>(left, 0));
      expectTimeout = expectTimeout < 0 ? leftMs : std::min(expectTimeout, leftMs);
    }

    std::vector<pollfd> fds;
    const int timeout = schedule.prepare(list, fds, now);
    check(timeout == expectTimeout, "poll timeout is the ceil of the earliest live deadline", step);

    for (pollfd& p : fds) {
      p.revents = pick(6) == 0 ? POLLIN : 0;
    }
    const Clock::time_point woke = now + std::chrono::microseconds(pick(12000));

    std::vector<Plan> plans(order.size());
    for (Plan& plan : plans) {
      plan = {.action = static_cast<Action>(pick(5)), .target = static_cast<std::size_t>(pick(kSlots))};
    }
    auto apply = [&](const Plan& plan) {
      Slot& t = slots[plan.target];
      switch (plan.action) {
      case Action::None:
        break;
      case Action::Destroy:
        if (t.live) {
          destroy(plan.target);
        }
        break;
      case Action::DestroyAndForget:
        if (t.live) {
          forget(plan.target);
          destroy(plan.target);
        }
        break;
      case Action::Forget:
        if (t.live && t.inLoop) {
          forget(plan.target);
        }
        break;
      case Action::Create:
        if (!t.live) {
          create(plan.target);
          t.inLoop = pick(2) == 0;
        }
        break;
      }
    };

    // The model pass: an entry runs while its source is the one listed, alive and in the loop, and it is woken.
    std::vector<Run> expected;
    std::vector<Run> actual;
    {
      std::vector<std::size_t> fdStart;
      std::size_t at = 0;
      for (std::size_t s : order) {
        fdStart.push_back(at);
        at += static_cast<std::size_t>(slots[s].get()->fds);
      }
      // The model changes only its own fields, never the objects; the real pass then starts from the same state.
      struct Saved {
        int id;
        bool live;
        bool inLoop;
        std::optional<Clock::time_point> deadline;
      };
      std::array<Saved, kSlots> saved{};
      for (std::size_t s = 0; s < kSlots; ++s) {
        saved[s] = {slots[s].id, slots[s].live, slots[s].inLoop, slots[s].deadline};
      }
      for (std::size_t k = 0; k < order.size(); ++k) {
        Slot& sl = slots[order[k]];
        if (!sl.live || !sl.inLoop || sl.id != passIds[k]) {
          continue;
        }
        const bool fd = std::any_of(fds.begin() + static_cast<std::ptrdiff_t>(fdStart[k]),
                                    fds.begin() + static_cast<std::ptrdiff_t>(fdStart[k] + sl.get()->fds),
                                    [](const pollfd& p) { return p.revents != 0; });
        const bool timedOut = sl.deadline && woke >= *sl.deadline;
        if (!fd && !timedOut) {
          continue;
        }
        sl.deadline.reset();
        expected.push_back({passIds[k], fd, timedOut});
        Plan plan = plans[k];
        Slot& t = slots[plan.target];
        if ((plan.action == Action::Destroy || plan.action == Action::DestroyAndForget) && t.live) {
          t.live = false;
          t.inLoop = false;
          t.deadline.reset();
        } else if (plan.action == Action::Forget && t.live && t.inLoop) {
          t.inLoop = false;
          t.deadline.reset();
        } else if (plan.action == Action::Create && !t.live) {
          t.live = true;
          t.id = -1;
        }
      }
      for (std::size_t s = 0; s < kSlots; ++s) {
        slots[s].id = saved[s].id;
        slots[s].live = saved[s].live;
        slots[s].inLoop = saved[s].inLoop;
        slots[s].deadline = saved[s].deadline;
      }
    }
    schedule.dispatch(fds, woke, [&](PollSource& source, std::size_t /*start*/, PollSchedule::Wake wake) {
      auto& f = static_cast<Fake&>(source);
      actual.push_back({f.id, wake.fd, wake.timeout});
      const auto k = static_cast<std::size_t>(std::find(passIds.begin(), passIds.end(), f.id) - passIds.begin());
      check(k < order.size() && slots[order[k]].live && slots[order[k]].id == f.id,
            "dispatch runs only a live source of this pass", step);
      if (k < order.size()) {
        slots[order[k]].deadline.reset();
        apply(plans[k]);
      }
    });
    check(actual == expected, "dispatch order, set and wake reasons match the model", step);
  }
  for (std::size_t s = 0; s < kSlots; ++s) {
    if (slots[s].live) {
      destroy(s);
    }
  }
  if (g_failures != 0) {
    std::fprintf(stderr, "%d failures\n", g_failures);
    return 1;
  }
  std::puts("poll_schedule: ok");
  return 0;
}
