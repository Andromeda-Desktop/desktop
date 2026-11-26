// Model test for LatestRequest as its consumers use it (activity action errors; a new action clears the old error):
// random request, cancel and reply events in any order, duplicates and replies after a cancel included. Invariant after every step: a shown error came
// from the newest request, and no cancel happened since that request began.
#include "core/latest_request.h"

#include <optional>
#include <print>
#include <random>
#include <string>
#include <vector>

namespace {

struct Pending {
  LatestRequest::Ticket ticket;
  int request;
};

struct Consumer {
  LatestRequest latest;
  std::string shown;
  std::optional<int> shownRequest;

  void clear() {
    latest.cancel();
    shown.clear();
    shownRequest.reset();
  }
  void report(const Pending& reply, const std::string& error) {
    if (!latest.current(reply.ticket) || error.empty()) {
      return;
    }
    shown = error;
    shownRequest = reply.request;
  }
};

} // namespace

int main() {
  int failures = 0;
  for (unsigned seed = 1; seed <= 400; ++seed) {
    std::mt19937 rng(seed);
    Consumer consumer;
    std::vector<Pending> pending;
    int requests = 0;
    int newest = 0;
    bool cancelledSinceNewest = true;
    for (int step = 0; step < 60; ++step) {
      const int event = static_cast<int>(rng() % 4);
      if (event == 0) {
        ++requests;
        consumer.clear();
        const auto ticket = consumer.latest.begin();
        for (int unit = 0; unit < 1 + static_cast<int>(rng() % 3); ++unit) {
          pending.push_back({ticket, requests});
        }
        newest = requests;
        cancelledSinceNewest = false;
      } else if (event == 1) {
        consumer.clear();
        cancelledSinceNewest = true;
      } else if (!pending.empty()) {
        const auto at = rng() % pending.size();
        const Pending reply = pending[at];
        if (rng() % 3 != 0) {
          pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(at));
        }
        consumer.report(reply, rng() % 2 == 0 ? "" : "error " + std::to_string(reply.request));
      }
      if (consumer.shownRequest && (*consumer.shownRequest != newest || cancelledSinceNewest)) {
        std::println(stderr, "FAIL seed {} step {}: shows request {}, newest {}, cancelled {}", seed, step,
                     *consumer.shownRequest, newest, cancelledSinceNewest);
        ++failures;
        break;
      }
    }
  }
  return failures == 0 ? 0 : 1;
}
