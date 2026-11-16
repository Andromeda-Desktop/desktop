#include "session_list.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <set>

using namespace term;
using namespace term::session_event;

namespace {

int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s (seed %u step %d)\n", __FILE__, __LINE__, #c, seed, step); ++failures; } } while (0)

unsigned seed = 0;
int step = 0;

void checkInvariants(const SessionList& list) {
  const auto& s = list.sessions();
  CHECK(s.empty() == !list.shown().has_value());
  std::set<std::uint64_t> ids;
  for (const auto& info : s) {
    ids.insert(info.id);
    if (info.seen == Seen::Unseen) {
      CHECK(std::holds_alternative<CommandFinished>(info.command));
      CHECK(list.shown() != info.id);
    }
  }
  CHECK(ids.size() == s.size());
  CHECK(std::set<std::uint64_t>(list.recent().begin(), list.recent().end()) == ids);
  CHECK(list.recent().size() == s.size());
  std::set<std::uint64_t> inSplit;
  for (const auto& split : list.splits()) {
    CHECK(split.size() >= 2 && split.size() <= kMaxPanes);
    for (const auto id : split) {
      CHECK(list.find(id) != nullptr);
      CHECK(inSplit.insert(id).second);
    }
  }
  const auto visible = list.visible();
  CHECK(visible.empty() == !list.shown().has_value());
  if (list.shown()) {
    CHECK(std::ranges::contains(visible, *list.shown()));
  }
  for (const auto id : visible) {
    CHECK(list.find(id)->seen == Seen::Seen);
  }
  if (list.shown()) {
    CHECK(list.find(*list.shown()) != nullptr);
    CHECK(list.find(*list.shown())->seen == Seen::Seen);
  }
}

SessionListEvent randomEvent(std::mt19937& rng, SessionClock::time_point now) {
  const std::uint64_t id = std::uniform_int_distribution<std::uint64_t>(1, 6)(rng);
  const std::uint64_t other = std::uniform_int_distribution<std::uint64_t>(1, 6)(rng);
  switch (std::uniform_int_distribution<int>(0, 12)(rng)) {
  case 0: return Open{id, SessionKind::Shell, "s", "~"};
  case 1: return Close{id};
  case 2: return Show{id};
  case 3: return CommandStart{id, "make", now};
  case 4: return CommandEnd{id, std::uniform_int_distribution<int>(0, 2)(rng) == 0 ? std::optional<int>(2) : std::optional<int>(0), now};
  case 5: return Rename{id, "r"};
  case 6: return Cwd{id, "/tmp"};
  case 7: return Kind{id, SessionKind::Ssh};
  case 8:
  case 9: return Join{id, other, std::uniform_int_distribution<int>(0, 1)(rng) == 0};
  case 10: return Output{id, std::uniform_int_distribution<int>(0, 1)(rng) == 0 ? CommandOutput::Active : CommandOutput::Quiet};
  default: return Leave{id};
  }
}

} // namespace

int main() {
  for (seed = 1; seed <= 300; ++seed) {
    std::mt19937 rng(seed);
    SessionList list;
    auto now = SessionClock::time_point{};
    for (step = 0; step < 400; ++step) {
      now += std::chrono::milliseconds(250);
      const SessionListEvent event = randomEvent(rng, now);
      const SessionList before = list;
      const auto shownBefore = list.shown();
      const ApplyResult result = list.apply(event);
      if (result == ApplyResult::Rejected) {
        CHECK(list == before);
      }
      CHECK((result == ApplyResult::LastClosed) == (std::holds_alternative<Close>(event) && result != ApplyResult::Rejected && list.sessions().empty()));
      if (result != ApplyResult::Rejected) {
        if (const auto* e = std::get_if<Open>(&event)) {
          CHECK(list.shown() == e->id);
        }
        if (const auto* e = std::get_if<Close>(&event); e && e->id == shownBefore && !list.sessions().empty()) {
          CHECK(list.shown() == before.recent()[1]);
        }
        if (const auto* e = std::get_if<Join>(&event)) {
          CHECK(list.splitOf(e->target) != nullptr && list.splitOf(e->target) == list.splitOf(e->joining));
          CHECK(list.shown() == e->joining);
          const auto* was = before.splitOf(e->target);
          CHECK(list.splitOf(e->target)->size() == (was ? was->size() + 1 : 2));
        }
        if (const auto* e = std::get_if<Leave>(&event)) {
          CHECK(list.splitOf(e->id) == nullptr);
          CHECK(list.shown() == shownBefore);
        }
        if (const auto* e = std::get_if<Join>(&event)) {
          const auto& panes = *list.splitOf(e->target);
          const auto at = [&](std::uint64_t id) { return std::ranges::find(panes, id) - panes.begin(); };
          CHECK(at(e->joining) == at(e->target) + (e->before ? -1 : 1));
        }
        if (const auto* e = std::get_if<CommandStart>(&event)) {
          CHECK(std::get<CommandRunning>(list.find(e->id)->command).output == CommandOutput::Active);
        }
        if (const auto* e = std::get_if<Output>(&event)) {
          const auto& was = std::get<CommandRunning>(before.find(e->id)->command);
          const auto& now = std::get<CommandRunning>(list.find(e->id)->command);
          CHECK(now.output == e->output && was.output != e->output);
          CHECK(now.command == was.command && now.start == was.start);
        }
        if (const auto* e = std::get_if<CommandEnd>(&event)) {
          CHECK((list.find(e->id)->seen == Seen::Unseen) == !std::ranges::contains(before.visible(), e->id));
        }
      }
      checkInvariants(list);
    }
  }
  if (failures == 0) {
    std::puts("session_list: ok");
  }
  return failures == 0 ? 0 : 1;
}
