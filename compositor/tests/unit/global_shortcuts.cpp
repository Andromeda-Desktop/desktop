#include "input/global_shortcuts.h"

#include "check.h"
#include "protocols/key_names.h"

#include <map>
#include <random>
#include <set>
#include <utility>

using andromeda::describeChord;
using andromeda::parsePortalTrigger;
using andromeda::preferredChordAllowed;
using andromeda::ShortcutChord;
using andromeda::ShortcutConflict;
using andromeda::ShortcutEvent;
using andromeda::ShortcutKeyTracker;
using andromeda::ShortcutRegistry;
using andromeda::ShortcutRequest;
using andromeda::ShortcutSessionId;
using andromeda::ShortcutSource;
namespace mod = andromeda::shortcut_mod;

namespace {

  ShortcutChord chord(uint32_t modifiers, xkb_keysym_t keysym) { return {.modifiers = modifiers, .keysym = keysym}; }

  const ShortcutChord kCtrlM = chord(mod::kCtrl, XKB_KEY_m);
  const ShortcutChord kCtrlN = chord(mod::kCtrl, XKB_KEY_n);
  const ShortcutChord kReserved = chord(mod::kCtrl | mod::kAlt, XKB_KEY_Delete);

  bool reserved(const ShortcutChord& c) { return c == kReserved; }

  ShortcutRequest request(std::string id, std::optional<ShortcutChord> preferred = std::nullopt) {
    return {.id = std::move(id), .description = {}, .preferred = preferred};
  }

  const andromeda::ShortcutState* state(const ShortcutRegistry& registry, ShortcutSessionId session, std::string_view id) {
    static std::vector<andromeda::ShortcutState> states;
    states = registry.shortcuts(session);
    for (const auto& s : states) {
      if (s.id == id) {
        return &s;
      }
    }
    return nullptr;
  }

} // namespace

ANDROMEDA_TEST(portalTriggersParseStrictly) {
  CHECK(parsePortalTrigger("CTRL+ALT+m") == chord(mod::kCtrl | mod::kAlt, XKB_KEY_m));
  CHECK(parsePortalTrigger("ctrl+shift+A") == chord(mod::kCtrl | mod::kShift, XKB_KEY_a));
  CHECK(parsePortalTrigger("LOGO+F5") == chord(mod::kLogo, XKB_KEY_F5));
  CHECK(parsePortalTrigger("NUM+Print") == chord(0, XKB_KEY_Print));
  CHECK(!parsePortalTrigger("CTRL+a;ignored"));
  CHECK(!parsePortalTrigger("CTRL+"));
  CHECK(!parsePortalTrigger("+a"));
  CHECK(!parsePortalTrigger(""));
  CHECK(!parsePortalTrigger("HYPER+a"));
  CHECK(!parsePortalTrigger("CTRL+Shift_L"));
  CHECK(!parsePortalTrigger("CTRL+nosuchkey"));
  CHECK_EQ(describeChord(chord(mod::kCtrl | mod::kAlt | mod::kShift | mod::kLogo, XKB_KEY_m)), std::string("Ctrl+Alt+Shift+Super+M"));
}

ANDROMEDA_TEST(preferredChordsCannotStealTyping) {
  CHECK(!preferredChordAllowed(chord(0, XKB_KEY_a)));
  CHECK(!preferredChordAllowed(chord(mod::kShift, XKB_KEY_a)));
  CHECK(preferredChordAllowed(chord(mod::kCtrl, XKB_KEY_a)));
  CHECK(preferredChordAllowed(chord(0, XKB_KEY_F13)));
  CHECK(preferredChordAllowed(chord(0, XKB_KEY_XF86AudioPlay)));
}

ANDROMEDA_TEST(approvalGrantsPreferredChordsOnlyWhenFree) {
  ShortcutRegistry registry(reserved);
  const ShortcutSessionId first = registry.open("org.first");
  const ShortcutSessionId second = registry.open("org.second");
  CHECK(registry.bind(first, {request("talk", kCtrlM), request("lock", kReserved)})
        == ShortcutRegistry::BindResult::NeedsApproval);
  CHECK(registry.bind(second, {request("talk", kCtrlM), request("typing", chord(0, XKB_KEY_a))})
        == ShortcutRegistry::BindResult::NeedsApproval);
  // Approvals run one at a time, in request order.
  CHECK(registry.pendingApproval() == first);
  CHECK(!registry.approve(second));
  CHECK(registry.approve(first));
  CHECK(registry.approve(second));

  CHECK(state(registry, first, "talk")->live());
  CHECK(state(registry, first, "lock")->conflict == ShortcutConflict::Unassigned);
  CHECK(state(registry, second, "talk")->conflict == ShortcutConflict::Unassigned);
  CHECK(state(registry, second, "typing")->conflict == ShortcutConflict::Unassigned);
  CHECK(registry.match(kCtrlM)->session == first);
}

ANDROMEDA_TEST(overridesWinAndEarlierApprovalKeepsAContestedChord) {
  ShortcutRegistry registry(reserved);
  registry.setGrants({{"org.first", {{"talk", kCtrlM}}}, {"org.second", {{"talk", kCtrlM}}}});
  const ShortcutSessionId first = registry.open("org.first");
  const ShortcutSessionId second = registry.open("org.second");
  CHECK(registry.bind(first, {request("talk")}) == ShortcutRegistry::BindResult::Bound);
  CHECK(registry.bind(second, {request("talk")}) == ShortcutRegistry::BindResult::Bound);
  CHECK(state(registry, second, "talk")->conflict == ShortcutConflict::InUse);

  registry.setOverrides({{"org.first", {{"talk", std::nullopt}}}, {"org.second", {{"talk", kCtrlN}}}});
  CHECK(state(registry, first, "talk")->conflict == ShortcutConflict::Disabled);
  CHECK(state(registry, second, "talk")->source == ShortcutSource::Config);
  CHECK(registry.match(kCtrlN)->session == second);
  CHECK(registry.match(kCtrlM) == nullptr);
}

ANDROMEDA_TEST(revokeAndCloseEndAHeldShortcut) {
  ShortcutRegistry registry(reserved);
  registry.setGrants({{"org.app", {{"talk", kCtrlM}}}});
  const ShortcutSessionId session = registry.open("org.app");
  CHECK(registry.bind(session, {request("talk")}) == ShortcutRegistry::BindResult::Bound);
  ShortcutKeyTracker tracker;
  std::vector<ShortcutEvent> events;
  CHECK(tracker.press(1, 50, kCtrlM, true, registry, 10, events));
  CHECK(tracker.press(1, 50, kCtrlM, true, registry, 11, events));
  registry.revoke("org.app");
  tracker.reconcile(registry, 12, events);
  CHECK(tracker.release(1, 50, 13, events));
  CHECK_EQ(events.size(), size_t{2});
  CHECK(events[0].kind == ShortcutEvent::Kind::Activated);
  CHECK(events[1].kind == ShortcutEvent::Kind::Deactivated);
  CHECK_EQ(events[1].timeMsec, uint32_t{12});
  CHECK(registry.phase(session) == ShortcutRegistry::Phase::Unbound);
  CHECK(registry.grants().empty());
}

// Random event sequences against the invariants of TASK-global-shortcuts.md (I1-I6).
ANDROMEDA_TEST(randomEventSequencesKeepTheShortcutInvariants) {
  const std::vector<std::string> apps{"org.a", "org.b", "org.c"};
  const std::vector<std::string> ids{"x", "y"};
  const std::vector<ShortcutChord> chords{kCtrlM, kCtrlN, kReserved, chord(0, XKB_KEY_F5), chord(0, XKB_KEY_a),
                                         chord(mod::kShift, XKB_KEY_space)};

  for (uint32_t seed = 1; seed <= 40; ++seed) {
    std::mt19937 random(seed);
    const auto pick = [&](size_t count) { return std::uniform_int_distribution<size_t>(0, count - 1)(random); };
    ShortcutRegistry registry(reserved);
    ShortcutKeyTracker tracker;
    std::vector<ShortcutSessionId> sessions;
    std::map<std::pair<ShortcutSessionId, std::string>, bool> active;
    std::set<std::pair<uintptr_t, uint32_t>> consumed;
    uint32_t time = 0;

    const auto apply = [&](const std::vector<ShortcutEvent>& events, bool allowed) {
      for (const ShortcutEvent& event : events) {
        bool& on = active[{event.session, event.id}];
        if (event.kind == ShortcutEvent::Kind::Activated) {
          CHECK(!on);
          CHECK(allowed);
          on = true;
        } else {
          CHECK(on);
          on = false;
        }
      }
    };

    for (int step = 0; step < 3000; ++step) {
      ++time;
      std::vector<ShortcutEvent> events;
      bool allowed = false;
      switch (pick(11)) {
      case 0:
        sessions.push_back(registry.open(apps[pick(apps.size())]));
        break;
      case 1:
        if (!sessions.empty()) {
          std::vector<ShortcutRequest> requests;
          for (const std::string& id : ids) {
            if (pick(3) != 0) {
              requests.push_back(request(id, chords[pick(chords.size())]));
            }
          }
          registry.bind(sessions[pick(sessions.size())], std::move(requests));
        }
        break;
      case 2:
        if (const auto pending = registry.pendingApproval()) {
          CHECK(registry.approve(*pending));
        }
        break;
      case 3:
        if (!sessions.empty()) {
          registry.cancel(sessions[pick(sessions.size())]);
        }
        break;
      case 4:
        if (!sessions.empty()) {
          const size_t index = pick(sessions.size());
          registry.close(sessions[index]);
          sessions.erase(sessions.begin() + static_cast<long>(index));
        }
        break;
      case 5:
        registry.revoke(pick(4) == 0 ? std::string() : apps[pick(apps.size())]);
        break;
      case 6: {
        andromeda::ShortcutOverrides overrides;
        if (pick(2) == 0) {
          overrides[apps[pick(apps.size())]][ids[pick(ids.size())]] =
              pick(3) == 0 ? std::nullopt : std::optional(chords[pick(chords.size())]);
        }
        registry.setOverrides(std::move(overrides));
        break;
      }
      case 7:
      case 8: {
        const uintptr_t keyboard = 1 + pick(2);
        const uint32_t keycode = 10 + static_cast<uint32_t>(pick(3));
        allowed = pick(4) != 0;
        const bool wasConsumed = consumed.contains({keyboard, keycode});
        const bool taken = tracker.press(keyboard, keycode, chords[pick(chords.size())], allowed, registry, time, events);
        // I5: a consumed key keeps consuming its repeats.
        CHECK(!wasConsumed || taken);
        if (taken) {
          consumed.insert({keyboard, keycode});
        }
        break;
      }
      case 9: {
        const uintptr_t keyboard = 1 + pick(2);
        const uint32_t keycode = 10 + static_cast<uint32_t>(pick(3));
        // I5: the release of a consumed press is consumed, and only then.
        CHECK_EQ(tracker.release(keyboard, keycode, time, events), consumed.erase({keyboard, keycode}) == 1);
        break;
      }
      default:
        if (pick(2) == 0) {
          tracker.suspend(time, events);
        } else {
          const uintptr_t keyboard = 1 + pick(2);
          tracker.removeKeyboard(keyboard, time, events);
          std::erase_if(consumed, [&](const auto& key) { return key.first == keyboard; });
        }
        break;
      }
      tracker.reconcile(registry, time, events);
      apply(events, allowed);

      // I1/I6: an active shortcut is always a live binding.
      for (const auto& [key, on] : active) {
        CHECK(!on || registry.live({.session = key.first, .id = key.second}));
      }
      // I3/I4: live chords are unique, never reserved, never type text, and come from a grant or an override.
      std::set<ShortcutChord> seen;
      for (const ShortcutSessionId session : registry.sessions()) {
        for (const auto& s : registry.shortcuts(session)) {
          if (!s.live()) {
            continue;
          }
          CHECK(registry.phase(session) == ShortcutRegistry::Phase::Bound);
          CHECK(s.chord.has_value() && !reserved(*s.chord) && !andromeda::chordTypesText(*s.chord));
          CHECK(seen.insert(*s.chord).second);
          CHECK(s.source == ShortcutSource::Config || s.source == ShortcutSource::Stored);
          if (s.source == ShortcutSource::Stored) {
            const auto& grants = registry.grants().at(*registry.app(session));
            CHECK(grants.at(s.id) == *s.chord);
          }
        }
      }
    }
    // I1: ending everything leaves no shortcut active.
    std::vector<ShortcutEvent> events;
    tracker.suspend(++time, events);
    apply(events, false);
    for (const auto& [key, on] : active) {
      CHECK(!on);
    }
  }
}

int main() { return RUN_TESTS(); }

ANDROMEDA_TEST(portalTriggersUseSharedKeyNames) {
  for (const auto& [alias, modifier] : key_names::kModifiers) {
    const uint32_t expected = modifier == key_names::Modifier::Shift ? mod::kShift
        : modifier == key_names::Modifier::Ctrl                      ? mod::kCtrl
        : modifier == key_names::Modifier::Alt                       ? mod::kAlt
                                                                     : mod::kLogo;
    CHECK(parsePortalTrigger(std::string(alias) + "+m") == chord(expected, XKB_KEY_m));
  }
  for (const auto& [alias, name] : key_names::kKeyAliases) {
    const xkb_keysym_t expected = xkb_keysym_from_name(std::string(name).c_str(), XKB_KEYSYM_NO_FLAGS);
    CHECK(parsePortalTrigger("CTRL+" + std::string(alias)) == chord(mod::kCtrl, xkb_keysym_to_lower(expected)));
  }
}
