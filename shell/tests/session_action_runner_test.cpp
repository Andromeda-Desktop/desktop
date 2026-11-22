#include "shell/session/session_action_runner.h"
#include "tests/test_check.h"
#include "tests/test_deferred.h"

#include <functional>
#include <thread>

namespace {

  // The runner keeps these references for logout and lock only, which this test never runs.
  CompositorPlatform& unusedPlatform() {
    static int storage = 0;
    return *reinterpret_cast<CompositorPlatform*>(&storage);
  }
  LockScreen& unusedLockScreen() {
    static int storage = 0;
    return *reinterpret_cast<LockScreen*>(&storage);
  }

  SessionPanelActionConfig action(const char* name) { return {.action = name}; }

} // namespace

int main() {
  const std::thread::id mainThread = std::this_thread::get_id();
  int rebootHooks = 0;
  int suspendArms = 0;
  bool armedOffMain = false;
  std::function<void(bool)> pendingDone;

  SessionActionHooks hooks;
  hooks.onReboot = [&](std::function<void(bool)> done) {
    ++rebootHooks;
    pendingDone = std::move(done);
  };
  hooks.onBeforePlainSuspend = [&] {
    ++suspendArms;
    armedOffMain = armedOffMain || std::this_thread::get_id() != mainThread;
  };
  SessionActionRunner runner(unusedPlatform(), unusedLockScreen(), hooks);
  // Overrides keep the test away from logind: each power call launches `true`.
  runner.setPowerConfig({.suspend = "true", .reboot = "true"});

  // One power action at a time: a second request while the reboot hook runs is refused.
  runner.invoke(action("reboot"));
  runner.invoke(action("reboot"));
  runner.invoke(action("suspend"));
  TEST_CHECK(rebootHooks == 1);
  TEST_CHECK(suspendArms == 0);

  // After the call returns to the main loop, the runner takes the next request.
  std::exchange(pendingDone, {})(true);
  TEST_CHECK(runDeferredUntil([&] {
    runner.invoke(action("suspend"));
    return suspendArms == 1;
  }));
  TEST_CHECK(runDeferredUntil([&] {
    runner.invoke(action("reboot"));
    return rebootHooks == 2;
  }));

  // The lock-before-sleep skip is main-loop state: it is never armed from the worker thread.
  TEST_CHECK(!armedOffMain);
  return 0;
}
