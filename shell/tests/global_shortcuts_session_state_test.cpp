#include "dbus/portal/global_shortcuts_session_state.h"
#include "tests/test_check.h"

#include <array>
#include <cstdint>
#include <random>
#include <set>
#include <string>

namespace {

  // The frontend, the request and the compositor session around the state, wired like GlobalShortcutsPortal.
  struct World {
    GlobalShortcutsSessionState state;
    bool requestOpen = false;
    int replies = 0;
    // Compositor side: is an approval pending, and which keys does it hold down for this session.
    bool compositorApproving = false;
    bool compositorBound = false;
    std::set<std::string> compositorHeld;
    // What the app saw.
    std::set<std::string> appHeld;
    bool appSawActivatedOutsideBound = false;

    void apply(const GlobalShortcutsActions& actions, GlobalShortcutsEvent event, const std::string& id) {
      if (actions.reply != GlobalShortcutsReply::None) {
        if (event == GlobalShortcutsEvent::BindCalled && actions.reply == GlobalShortcutsReply::Failed) {
          return;
        }
        TEST_CHECK(requestOpen);
        requestOpen = false;
        ++replies;
      }
      for (const auto& [heldId, time] : actions.release) {
        TEST_CHECK(appHeld.erase(heldId) == 1);
      }
      if (actions.emitActivated) {
        appSawActivatedOutsideBound |= state.phase() != GlobalShortcutsPhase::Bound;
        TEST_CHECK(appHeld.insert(id).second);
      }
      if (actions.emitDeactivated) {
        TEST_CHECK(appHeld.erase(id) == 1);
      }
      if (actions.sendBind) {
        TEST_CHECK(!compositorApproving);
        compositorApproving = true;
        compositorBound = false;
      }
      if (actions.reopen) {
        compositorApproving = false;
        compositorBound = false;
        compositorHeld.clear();
      }
    }

    void send(GlobalShortcutsEvent event, const std::string& id = {}) { apply(state.handle(event, id, 7), event, id); }

    void run(int step, const std::string& id) {
      switch (step) {
      case 0:
        if (state.phase() == GlobalShortcutsPhase::Approving || state.phase() == GlobalShortcutsPhase::Closed) {
          const auto actions = state.handle(GlobalShortcutsEvent::BindCalled);
          TEST_CHECK(actions.reply == GlobalShortcutsReply::Failed && !actions.sendBind);
          return;
        }
        requestOpen = true;
        send(GlobalShortcutsEvent::BindCalled);
        return;
      case 1:
        if (compositorApproving) {
          compositorApproving = false;
          compositorBound = true;
        }
        if (compositorBound) {
          send(GlobalShortcutsEvent::Done);
        }
        return;
      case 2:
        if (compositorApproving) {
          compositorApproving = false;
          send(GlobalShortcutsEvent::Cancelled);
        }
        return;
      case 3:
        if (compositorBound) {
          compositorBound = false;
          releaseAll();
          send(GlobalShortcutsEvent::Revoked);
        }
        return;
      case 4:
        if (compositorBound && compositorHeld.insert(id).second) {
          send(GlobalShortcutsEvent::Activated, id);
        }
        return;
      case 5:
        if (compositorHeld.erase(id) == 1) {
          send(GlobalShortcutsEvent::Deactivated, id);
        }
        return;
      case 6:
        if (requestOpen) {
          send(GlobalShortcutsEvent::RequestClosed);
        }
        return;
      case 7:
        send(GlobalShortcutsEvent::SessionClosed);
        return;
      default:
        // A bind replaces the binding; the compositor ends every held key of the old one.
        if (compositorApproving) {
          releaseAll();
        }
        return;
      }
    }

    void releaseAll() {
      for (const std::string& held : std::set(compositorHeld)) {
        compositorHeld.erase(held);
        send(GlobalShortcutsEvent::Deactivated, held);
      }
    }
  };

  void checkInvariants(const World& world) {
    const GlobalShortcutsPhase phase = world.state.phase();
    // One reply per BindShortcuts: a request is open exactly while an approval is pending.
    TEST_CHECK(world.requestOpen == (phase == GlobalShortcutsPhase::Approving));
    TEST_CHECK(!world.appSawActivatedOutsideBound);
    // The app never sees a key held that the compositor released, unless the session is closed.
    if (phase != GlobalShortcutsPhase::Closed) {
      for (const std::string& id : world.appHeld) {
        TEST_CHECK(world.compositorHeld.contains(id));
        TEST_CHECK(world.state.held(id));
      }
    }
  }

} // namespace

int main() {
  const std::array<std::string, 3> ids{"a", "b", "c"};
  std::mt19937 rng(20261117);
  std::uniform_int_distribution<int> pickStep(0, 8);
  std::uniform_int_distribution<std::size_t> pickId(0, ids.size() - 1);
  for (int run = 0; run < 3000; ++run) {
    World world;
    bool closed = false;
    for (int i = 0; i < 80; ++i) {
      world.run(pickStep(rng), ids[pickId(rng)]);
      checkInvariants(world);
      // Closed is final.
      TEST_CHECK(!closed || world.state.phase() == GlobalShortcutsPhase::Closed);
      closed = world.state.phase() == GlobalShortcutsPhase::Closed;
    }
  }
  return 0;
}
