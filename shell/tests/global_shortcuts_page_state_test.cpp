#include "shell/settings/global_shortcuts_page_state.h"
#include "tests/test_check.h"

#include <nlohmann/json.hpp>

#include <array>
#include <random>
#include <string>
#include <variant>
#include <vector>

using namespace settings::global_shortcuts;

namespace {

  template <class... F> struct Overload : F... {
    using F::operator()...;
  };

  std::vector<App> sampleApps(std::mt19937& rng) {
    std::vector<App> apps;
    if (rng() % 4 != 0) {
      apps.push_back(App{.id = "a", .running = true, .shortcuts = {Shortcut{.id = "talk"}, Shortcut{.id = "mute"}}});
    }
    if (rng() % 2 != 0) {
      apps.push_back(App{.id = "b", .running = false, .shortcuts = {Shortcut{.id = "talk"}}});
    }
    return apps;
  }

  // The window and the compositor around the state: it runs every action at once, as the synchronous IPC does.
  struct World {
    State state;
    std::mt19937 rng{20261117};
    bool captureOutstanding = false;
    bool lastWriteFailed = false;
    bool anyWrite = false;
    std::vector<App> served;
    std::string lastCaptured;

    void run(std::vector<Action> actions) {
      for (auto& action : actions) {
        std::visit(
            Overload{
                [this](Capture&) {
                  TEST_CHECK(!captureOutstanding);
                  TEST_CHECK(std::holds_alternative<Recording>(state.edit));
                  captureOutstanding = true;
                },
                [this](CancelCapture&) { captureOutstanding = false; },
                [this](Assign& assign) {
                  const auto* recording = std::get_if<Recording>(&state.edit);
                  TEST_CHECK(recording != nullptr);
                  TEST_CHECK(!lastCaptured.empty());
                  TEST_CHECK(assign.app == recording->app && assign.id == recording->id);
                  TEST_CHECK(assign.chord == (lastCaptured == kTurnOffChord ? std::string{} : lastCaptured));
                  const unsigned roll = rng() % 4;
                  const auto result = roll == 0   ? Assigned::Result::Reserved
                                      : roll == 1 ? Assigned::Result::InUse
                                      : roll == 2 ? Assigned::Result::Failed
                                                  : Assigned::Result::Ok;
                  if (result == Assigned::Result::Ok || result == Assigned::Result::Failed) {
                    anyWrite = true;
                    lastWriteFailed = result == Assigned::Result::Failed;
                  }
                  run(state.handle(Assigned{.result = result, .holder = "b"}));
                },
                [this](Revoke& revoke) {
                  const auto* confirm = std::get_if<Confirm>(&state.edit);
                  TEST_CHECK(confirm != nullptr && confirm->app == revoke.app);
                  const bool ok = rng() % 3 != 0;
                  anyWrite = true;
                  lastWriteFailed = !ok;
                  if (ok) {
                    std::erase_if(served, [&](const App& app) { return revoke.app.empty() || app.id == revoke.app; });
                  }
                  run(state.handle(Removed{.ok = ok}));
                },
                [this](Reload&) { run(state.handle(Loaded{.apps = served})); },
            },
            action
        );
      }
    }

    void checkInvariants() {
      const auto* recording = std::get_if<Recording>(&state.edit);
      // At most one recorder, and it waits for keys exactly while the compositor captures.
      TEST_CHECK((recording != nullptr && recording->pending.empty()) == captureOutstanding);
      if (recording != nullptr) {
        TEST_CHECK(state.load == Load::Ready);
      }
      if (anyWrite) {
        TEST_CHECK(state.failure.has_value() == lastWriteFailed);
      }
    }
  };

} // namespace

int main() {
  // The reply parser: sessions win over grants for the same id, grant-only apps are listed, InUse names its holder.
  {
    const auto ok = nlohmann::json::parse(R"({
      "grants": {"a": {"talk": "Ctrl+F9"}, "c": {"x": "Ctrl+F1"}},
      "sessions": [
        {"app": "a", "phase": "bound", "shortcuts": [
          {"id": "talk", "description": "Talk", "trigger": "Ctrl+F9", "source": "stored", "conflict": "none"}]},
        {"app": "b", "phase": "bound", "shortcuts": [
          {"id": "talk", "description": "Talk", "trigger": "Ctrl+F9", "source": "config", "conflict": "in_use"},
          {"id": "off", "description": "", "trigger": null, "source": "config", "conflict": "disabled"}]}
      ]})");
    const auto apps = parseApps(ok);
    TEST_CHECK(apps.has_value());
    TEST_CHECK(apps->size() == 3);
    TEST_CHECK((*apps)[0].id == "a" && (*apps)[0].shortcuts.size() == 1 && (*apps)[0].running);
    TEST_CHECK((*apps)[1].shortcuts[0].holder == "a");
    TEST_CHECK((*apps)[1].notWorking() == 2);
    TEST_CHECK((*apps)[2].id == "c" && !(*apps)[2].running && (*apps)[2].shortcuts[0].trigger == "Ctrl+F1");
    TEST_CHECK(!parseApps(nlohmann::json::parse(R"({"sessions": [{"app": 3}]})")).has_value());
  }

  // Random event sequences: the invariants hold after every event.
  World world;
  constexpr std::array<const char*, 4> kChords{"", "BackSpace", "Ctrl+F9", "Ctrl+Alt+T"};
  constexpr std::array<const char*, 3> kApps{"a", "b", ""};
  constexpr std::array<const char*, 3> kIds{"talk", "mute", "gone"};
  for (int step = 0; step < 200000; ++step) {
    auto& rng = world.rng;
    Event event = Leave{};
    switch (rng() % 7) {
    case 0:
      world.served = sampleApps(rng);
      event = Loaded{.apps = rng() % 8 == 0 ? std::nullopt : std::optional(world.served)};
      break;
    case 1:
      event = Record{.app = kApps[rng() % 2], .id = kIds[rng() % kIds.size()]};
      break;
    case 2:
      if (!world.captureOutstanding) {
        continue;
      }
      world.captureOutstanding = false;
      world.lastCaptured = kChords[rng() % kChords.size()];
      event = Captured{.chord = world.lastCaptured};
      break;
    case 3:
      event = AskRemove{.app = kApps[rng() % kApps.size()]};
      break;
    case 4:
    case 5:
      event = Answer{.remove = rng() % 2 == 0};
      break;
    default:
      if (rng() % 10 != 0) {
        continue;
      }
      break;
    }
    const bool wasLoaded = std::holds_alternative<Loaded>(event);
    if (std::holds_alternative<Leave>(event)) {
      world.lastWriteFailed = false;
    }
    world.run(world.state.handle(std::move(event)));
    if (wasLoaded) {
      TEST_CHECK(world.state.apps.size() == (world.state.load == Load::Ready ? world.served.size() : 0));
    }
    world.checkInvariants();
  }
  return 0;
}
