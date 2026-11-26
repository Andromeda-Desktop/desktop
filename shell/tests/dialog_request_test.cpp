#include "core/deferred_call.h"
#include "tests/test_check.h"
#include "ui/dialogs/dialog_request.h"

#include <map>
#include <optional>
#include <random>
#include <string>

namespace {

  struct Options {
    std::string title;
  };

  using Request = DialogRequest<Options, int>;

  void drain() {
    DeferredCall::drainWakeFd();
    for (auto& fn : DeferredCall::takePending()) {
      fn();
    }
  }

  // Random open (presented or refused), settle, cancel and loop turns. Every request ends exactly once, a settle
  // hands its value only to the request pending at that moment, and options show only the pending request.
  void runModel(Request::Delivery delivery, unsigned seed) {
    Request request(delivery);
    std::mt19937 rng(seed);
    std::map<int, int> ends;
    std::map<int, std::optional<int>> results;
    std::optional<int> pending;
    std::map<int, std::optional<int>> expected;
    int next = 0;
    for (int step = 0; step < 400; ++step) {
      switch (rng() % 4) {
      case 0: {
        const int id = next++;
        const bool presented = rng() % 3 != 0;
        if (pending) {
          expected[*pending] = std::nullopt;
        }
        const bool opened = request.open(
            Options{std::to_string(id)},
            [&ends, &results, id](std::optional<int> result) {
              ++ends[id];
              results[id] = result;
            },
            [presented] { return presented; }
        );
        TEST_CHECK(opened == presented);
        pending = presented ? std::optional<int>(id) : std::nullopt;
        if (!presented) {
          expected[id] = std::nullopt;
        }
        break;
      }
      case 1: {
        const int value = static_cast<int>(rng() % 1000);
        auto deliver = request.settle(value);
        if (pending) {
          expected[*pending] = value;
        }
        pending.reset();
        deliver();
        break;
      }
      case 2:
        request.cancel();
        if (pending) {
          expected[*pending] = std::nullopt;
        }
        pending.reset();
        break;
      default:
        drain();
        break;
      }
      TEST_CHECK(request.options().title == (pending ? std::to_string(*pending) : std::string()));
      for (const auto& [id, count] : ends) {
        TEST_CHECK(count == 1);
      }
    }
    request.cancel();
    if (pending) {
      expected[*pending] = std::nullopt;
    }
    drain();
    TEST_CHECK(static_cast<int>(ends.size()) == next);
    for (const auto& [id, result] : expected) {
      TEST_CHECK(ends[id] == 1);
      TEST_CHECK(results[id] == result);
    }
  }

} // namespace

int main() {
  for (unsigned seed = 1; seed <= 50; ++seed) {
    runModel(Request::Delivery::Now, seed);
    runModel(Request::Delivery::Deferred, seed);
  }
  return 0;
}
