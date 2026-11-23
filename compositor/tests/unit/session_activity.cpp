#include "check.h"
#include "core/session_activity.h"

#include <random>

using namespace andromeda;

namespace {

  constexpr int kEvents = 7;

  // The oracle: the session's facts as separate inputs, the way the AR2 design states them.
  struct Facts {
    bool lockRequested = false; // a lock client or the blank holds the session
    bool restartPending = false;
    bool suspended = false;
    bool userQuiet = false;
  };

} // namespace

// Random event sequences against the design rules: unlock only from Locked, suspend only while locked, a restart
// lock leaves only through a lock client, and input never changes a locked phase.
ANDROMEDA_TEST(randomEventSequencesKeepTheSessionInvariants) {
  std::mt19937 random(17);
  for (int run = 0; run < 2000; ++run) {
    SessionActivity activity = SessionActivity::Active;
    Facts facts;
    for (int step = 0; step < 40; ++step) {
      const auto event = static_cast<SessionEvent>(random() % kEvents);
      const std::optional<SessionActivity> next = nextActivity(activity, event);
      const bool wasLocked = locked(activity);
      switch (event) {
      case SessionEvent::Input:
      case SessionEvent::IdleTimeout:
        CHECK(next.has_value());
        CHECK_EQ(locked(*next), wasLocked);
        if (wasLocked) {
          CHECK(*next == activity);
        } else {
          facts.userQuiet = event == SessionEvent::IdleTimeout;
        }
        break;
      case SessionEvent::RestartLock:
        CHECK_EQ(next.has_value(), !wasLocked);
        if (next) {
          facts = {.lockRequested = true, .restartPending = true};
        }
        break;
      case SessionEvent::Lock:
        CHECK_EQ(next.has_value(), !facts.lockRequested || facts.restartPending);
        if (next) {
          facts = {.lockRequested = true};
        }
        break;
      case SessionEvent::Unlock:
        CHECK_EQ(next.has_value(), facts.lockRequested && !facts.restartPending && !facts.suspended);
        if (next) {
          facts = {};
        }
        break;
      case SessionEvent::Sleep:
        CHECK_EQ(next.has_value(), facts.lockRequested && !facts.restartPending && !facts.suspended);
        if (next) {
          facts.suspended = true;
        }
        break;
      case SessionEvent::Resume:
        CHECK_EQ(next.has_value(), facts.suspended);
        if (next) {
          facts.suspended = false;
        }
        break;
      }
      if (next) {
        activity = *next;
      }
      CHECK_EQ(locked(activity), facts.lockRequested);
      CHECK_EQ(activity == SessionActivity::RestartLocked, facts.restartPending);
      CHECK_EQ(activity == SessionActivity::Sleeping, facts.suspended);
      if (!facts.lockRequested) {
        CHECK_EQ(activity == SessionActivity::Idle, facts.userQuiet);
      }
    }
  }
}

int main() { return RUN_TESTS(); }
