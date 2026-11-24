#include "input/global_shortcuts.h"

#include "protocols/key_names.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>
#include <utility>

namespace andromeda {

  namespace {

    std::string upper(std::string_view text) {
      std::string out(text);
      std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
      return out;
    }

    // Keysyms that only modify other keys: a chord of modifiers alone never fires.
    bool modifierKeysym(xkb_keysym_t keysym) {
      return (keysym >= XKB_KEY_Shift_L && keysym <= XKB_KEY_Hyper_R)
             || (keysym >= XKB_KEY_ISO_Lock && keysym <= XKB_KEY_ISO_Last_Group_Lock)
             || keysym == XKB_KEY_Mode_switch
             || keysym == XKB_KEY_Num_Lock;
    }

    std::optional<uint32_t> portalModifier(std::string_view token) {
      // The XDG shortcuts spec lists NUM (Num Lock); matching ignores lock state, so it adds nothing.
      if (upper(token) == "NUM") {
        return 0U;
      }
      const auto modifier = key_names::modifier(token);
      if (!modifier) {
        return std::nullopt;
      }
      switch (*modifier) {
      case key_names::Modifier::Shift:
        return shortcut_mod::kShift;
      case key_names::Modifier::Ctrl:
        return shortcut_mod::kCtrl;
      case key_names::Modifier::Alt:
        return shortcut_mod::kAlt;
      case key_names::Modifier::Super:
        return shortcut_mod::kLogo;
      }
      return std::nullopt;
    }

  } // namespace

  std::optional<ShortcutChord> parsePortalTrigger(std::string_view text) {
    std::vector<std::string_view> tokens;
    size_t start = 0;
    while (true) {
      const size_t plus = text.find('+', start);
      tokens.push_back(text.substr(start, plus == std::string_view::npos ? std::string_view::npos : plus - start));
      if (plus == std::string_view::npos) {
        break;
      }
      start = plus + 1;
    }
    for (const std::string_view token : tokens) {
      if (token.empty() || !std::ranges::all_of(token, [](unsigned char c) { return std::isalnum(c) || c == '_'; })) {
        return std::nullopt;
      }
    }
    ShortcutChord chord;
    for (size_t index = 0; index + 1 < tokens.size(); ++index) {
      const auto modifier = portalModifier(tokens[index]);
      if (!modifier) {
        return std::nullopt;
      }
      chord.modifiers |= *modifier;
    }
    const std::string name(key_names::keysymName(tokens.back()));
    const xkb_keysym_t keysym = xkb_keysym_from_name(name.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
    if (keysym == XKB_KEY_NoSymbol || modifierKeysym(keysym)) {
      return std::nullopt;
    }
    chord.keysym = xkb_keysym_to_lower(keysym);
    return chord;
  }

  std::string describeChord(const ShortcutChord& chord) {
    std::string out;
    constexpr std::array<std::pair<uint32_t, std::string_view>, 4> kNames{{
        {shortcut_mod::kCtrl, "Ctrl"},
        {shortcut_mod::kAlt, "Alt"},
        {shortcut_mod::kShift, "Shift"},
        {shortcut_mod::kLogo, "Super"},
    }};
    for (const auto& [bit, name] : kNames) {
      if ((chord.modifiers & bit) != 0) {
        out += name;
        out += '+';
      }
    }
    std::array<char, 64> name{};
    const xkb_keysym_t shown = xkb_keysym_to_upper(chord.keysym);
    if (xkb_keysym_get_name(shown, name.data(), name.size()) > 0) {
      out += name.data();
    }
    return out;
  }

  bool preferredChordAllowed(const ShortcutChord& chord) {
    if ((chord.modifiers & (shortcut_mod::kCtrl | shortcut_mod::kAlt | shortcut_mod::kLogo)) != 0) {
      return true;
    }
    const xkb_keysym_t key = chord.keysym;
    constexpr xkb_keysym_t kVendorFirst = 0x1008FE01;
    constexpr xkb_keysym_t kVendorLast = 0x1008FFFF;
    return (key >= XKB_KEY_F1 && key <= XKB_KEY_F35)
           || key == XKB_KEY_Print
           || key == XKB_KEY_Pause
           || key == XKB_KEY_Scroll_Lock
           || key == XKB_KEY_Menu
           || (key >= kVendorFirst && key <= kVendorLast);
  }

  ShortcutRegistry::ShortcutRegistry(std::function<bool(const ShortcutChord&)> reserved) :
      m_reserved(std::move(reserved)) {}

  ShortcutSessionId ShortcutRegistry::open(std::string app) {
    const ShortcutSessionId id = m_nextSession++;
    Session session;
    session.app = std::move(app);
    m_sessions.emplace(id, std::move(session));
    return id;
  }

  bool ShortcutRegistry::transition(Session& session, Phase next) {
    const Phase from = session.phase;
    const bool valid = (from == Phase::Unbound && (next == Phase::Approving || next == Phase::Bound))
                       || (from == Phase::Approving && (next == Phase::Bound || next == Phase::Unbound))
                       || (from == Phase::Bound && (next == Phase::Approving || next == Phase::Bound || next == Phase::Unbound));
    if (!valid) {
      return false;
    }
    if (next == Phase::Bound && from != Phase::Bound) {
      session.boundOrder = m_nextOrder++;
    }
    session.phase = next;
    return true;
  }

  bool ShortcutRegistry::covered(const Session& session) const {
    const auto grants = m_grants.find(session.app);
    const auto overrides = m_overrides.find(session.app);
    return std::ranges::all_of(session.shortcuts, [&](const ShortcutRequest& request) {
      return (grants != m_grants.end() && grants->second.contains(request.id))
             || (overrides != m_overrides.end() && overrides->second.contains(request.id));
    });
  }

  ShortcutRegistry::BindResult ShortcutRegistry::bind(ShortcutSessionId id, std::vector<ShortcutRequest> shortcuts) {
    const auto found = m_sessions.find(id);
    if (found == m_sessions.end() || found->second.phase == Phase::Approving) {
      return BindResult::Rejected;
    }
    std::set<std::string_view> ids;
    for (const ShortcutRequest& request : shortcuts) {
      if (request.id.empty() || !ids.insert(request.id).second) {
        return BindResult::Rejected;
      }
    }
    Session& session = found->second;
    session.shortcuts = std::move(shortcuts);
    if (covered(session)) {
      transition(session, Phase::Bound);
      resolve();
      return BindResult::Bound;
    }
    transition(session, Phase::Approving);
    m_approvals.push_back(id);
    resolve();
    return BindResult::NeedsApproval;
  }

  bool ShortcutRegistry::chordTaken(const ShortcutChord& chord) const {
    if (m_live.contains(chord)) {
      return true;
    }
    for (const auto& [app, grants] : m_grants) {
      if (std::ranges::any_of(grants, [&](const auto& entry) { return entry.second == chord; })) {
        return true;
      }
    }
    for (const auto& [app, overrides] : m_overrides) {
      if (std::ranges::any_of(overrides, [&](const auto& entry) { return entry.second == chord; })) {
        return true;
      }
    }
    return false;
  }

  bool ShortcutRegistry::approve(ShortcutSessionId id) {
    if (m_approvals.empty() || m_approvals.front() != id) {
      return false;
    }
    Session& session = m_sessions.at(id);
    auto& grants = m_grants[session.app];
    const auto overrides = m_overrides.find(session.app);
    for (const ShortcutRequest& request : session.shortcuts) {
      if (grants.contains(request.id) || (overrides != m_overrides.end() && overrides->second.contains(request.id))) {
        continue;
      }
      const auto& chord = request.preferred;
      if (chord && preferredChordAllowed(*chord) && !(m_reserved && m_reserved(*chord)) && !chordTaken(*chord)) {
        grants.emplace(request.id, *chord);
      }
    }
    if (grants.empty()) {
      m_grants.erase(session.app);
    }
    m_approvals.pop_front();
    transition(session, Phase::Bound);
    resolve();
    return true;
  }

  bool ShortcutRegistry::cancel(ShortcutSessionId id) {
    const auto found = m_sessions.find(id);
    if (found == m_sessions.end() || !transition(found->second, Phase::Unbound)) {
      return false;
    }
    std::erase(m_approvals, id);
    resolve();
    return true;
  }

  void ShortcutRegistry::close(ShortcutSessionId id) {
    std::erase(m_approvals, id);
    if (m_sessions.erase(id) != 0) {
      resolve();
    }
  }

  void ShortcutRegistry::revoke(std::string_view app) {
    if (app.empty()) {
      m_grants.clear();
    } else {
      m_grants.erase(std::string(app));
    }
    for (auto& [id, session] : m_sessions) {
      if (session.phase == Phase::Bound && (app.empty() || session.app == app)) {
        transition(session, Phase::Unbound);
      }
    }
    resolve();
  }

  void ShortcutRegistry::setGrants(ShortcutGrants grants) {
    m_grants = std::move(grants);
    resolve();
  }

  void ShortcutRegistry::setOverrides(ShortcutOverrides overrides) {
    m_overrides = std::move(overrides);
    resolve();
  }

  void ShortcutRegistry::setReserved(std::function<bool(const ShortcutChord&)> reserved) {
    m_reserved = std::move(reserved);
    resolve();
  }

  void ShortcutRegistry::resolve() {
    std::vector<std::pair<ShortcutSessionId, Session*>> bound;
    for (auto& [id, session] : m_sessions) {
      if (session.phase == Phase::Bound) {
        bound.emplace_back(id, &session);
      }
    }
    std::ranges::sort(bound, {}, [](const auto& entry) { return entry.second->boundOrder; });

    std::map<ShortcutChord, Binding> live;
    bool changed = false;
    for (auto& [id, session] : m_sessions) {
      std::vector<ShortcutState> states;
      states.reserve(session.shortcuts.size());
      for (const ShortcutRequest& request : session.shortcuts) {
        states.push_back({
            .id = request.id,
            .description = request.description,
            .chord = std::nullopt,
            .source = ShortcutSource::None,
            .conflict = ShortcutConflict::Unassigned,
        });
      }
      if (session.phase == Phase::Bound) {
        const auto grants = m_grants.find(session.app);
        const auto overrides = m_overrides.find(session.app);
        for (ShortcutState& state : states) {
          if (overrides != m_overrides.end()) {
            if (const auto entry = overrides->second.find(state.id); entry != overrides->second.end()) {
              state.source = ShortcutSource::Config;
              state.chord = entry->second;
              state.conflict = entry->second ? ShortcutConflict::None : ShortcutConflict::Disabled;
            }
          }
          if (state.source == ShortcutSource::None && grants != m_grants.end()) {
            if (const auto entry = grants->second.find(state.id); entry != grants->second.end()) {
              state.source = ShortcutSource::Stored;
              state.chord = entry->second;
              state.conflict = ShortcutConflict::None;
            }
          }
        }
      }
      changed |= states.size() != session.states.size()
                 || !std::ranges::equal(states, session.states, [](const ShortcutState& a, const ShortcutState& b) {
                      return a.id == b.id && a.description == b.description && a.chord == b.chord && a.source == b.source;
                    });
      session.states = std::move(states);
    }
    // Earlier approvals keep a contested chord.
    for (const auto& [id, session] : bound) {
      for (ShortcutState& state : session->states) {
        const ShortcutConflict before = state.conflict;
        if (state.chord && state.conflict == ShortcutConflict::None) {
          if (m_reserved && m_reserved(*state.chord)) {
            state.conflict = ShortcutConflict::Reserved;
          } else if (!live.try_emplace(*state.chord, Binding{.session = id, .id = state.id}).second) {
            state.conflict = ShortcutConflict::InUse;
          }
        }
        changed |= before != state.conflict;
      }
    }
    if (live != m_live) {
      m_live = std::move(live);
      changed = true;
    }
    if (changed) {
      ++m_generation;
    }
  }

  ShortcutRegistry::Phase ShortcutRegistry::phase(ShortcutSessionId id) const {
    const auto found = m_sessions.find(id);
    return found == m_sessions.end() ? Phase::Closed : found->second.phase;
  }

  std::optional<ShortcutSessionId> ShortcutRegistry::pendingApproval() const {
    if (m_approvals.empty()) {
      return std::nullopt;
    }
    return m_approvals.front();
  }

  const std::string* ShortcutRegistry::app(ShortcutSessionId id) const {
    const auto found = m_sessions.find(id);
    return found == m_sessions.end() ? nullptr : &found->second.app;
  }

  std::vector<ShortcutState> ShortcutRegistry::shortcuts(ShortcutSessionId id) const {
    const auto found = m_sessions.find(id);
    return found == m_sessions.end() ? std::vector<ShortcutState>{} : found->second.states;
  }

  std::vector<ShortcutSessionId> ShortcutRegistry::sessions() const {
    std::vector<ShortcutSessionId> ids;
    ids.reserve(m_sessions.size());
    for (const auto& [id, session] : m_sessions) {
      ids.push_back(id);
    }
    return ids;
  }

  const ShortcutRegistry::Binding* ShortcutRegistry::match(const ShortcutChord& chord) const {
    const auto found = m_live.find(chord);
    return found == m_live.end() ? nullptr : &found->second;
  }

  std::optional<std::string>
  ShortcutRegistry::holder(const ShortcutChord& chord, std::string_view app, std::string_view id) const {
    const auto other = [&](const std::string& ownerApp, const std::string& ownerId) {
      return ownerApp != app || ownerId != id;
    };
    if (const auto found = m_live.find(chord); found != m_live.end()) {
      const std::string& owner = m_sessions.at(found->second.session).app;
      if (other(owner, found->second.id)) {
        return owner;
      }
    }
    for (const auto& [ownerApp, overrides] : m_overrides) {
      for (const auto& [ownerId, overridden] : overrides) {
        if (overridden == chord && other(ownerApp, ownerId)) {
          return ownerApp;
        }
      }
    }
    for (const auto& [ownerApp, grants] : m_grants) {
      const auto overrides = m_overrides.find(ownerApp);
      for (const auto& [ownerId, granted] : grants) {
        const bool overridden = overrides != m_overrides.end() && overrides->second.contains(ownerId);
        if (granted == chord && !overridden && other(ownerApp, ownerId)) {
          return ownerApp;
        }
      }
    }
    return std::nullopt;
  }

  bool ShortcutRegistry::live(const Binding& binding) const {
    return std::ranges::any_of(m_live, [&](const auto& entry) { return entry.second == binding; });
  }

  void ShortcutKeyTracker::end(Entry& entry, uint32_t timeMsec, std::vector<ShortcutEvent>& events) {
    if (entry.phase != KeyPhase::Held) {
      return;
    }
    events.push_back({
        .kind = ShortcutEvent::Kind::Deactivated,
        .session = entry.binding.session,
        .id = entry.binding.id,
        .timeMsec = timeMsec,
    });
    entry.phase = KeyPhase::Swallowed;
  }

  bool ShortcutKeyTracker::press(
      Keyboard keyboard, uint32_t keycode, const ShortcutChord& chord, bool allowed, const ShortcutRegistry& registry,
      uint32_t timeMsec, std::vector<ShortcutEvent>& events
  ) {
    const Key key{.keyboard = keyboard, .keycode = keycode};
    if (m_keys.contains(key)) {
      return true;
    }
    if (!allowed) {
      return false;
    }
    const ShortcutRegistry::Binding* binding = registry.match(chord);
    if (binding == nullptr) {
      return false;
    }
    // One binding is held by at most one key: a second keyboard pressing the same chord is swallowed.
    const bool alreadyHeld = std::ranges::any_of(m_keys, [&](const auto& entry) {
      return entry.second.phase == KeyPhase::Held && entry.second.binding == *binding;
    });
    m_keys.emplace(key, Entry{.phase = alreadyHeld ? KeyPhase::Swallowed : KeyPhase::Held, .binding = *binding});
    if (!alreadyHeld) {
      events.push_back({
          .kind = ShortcutEvent::Kind::Activated,
          .session = binding->session,
          .id = binding->id,
          .timeMsec = timeMsec,
      });
    }
    return true;
  }

  bool ShortcutKeyTracker::release(Keyboard keyboard, uint32_t keycode, uint32_t timeMsec, std::vector<ShortcutEvent>& events) {
    const auto found = m_keys.find(Key{.keyboard = keyboard, .keycode = keycode});
    if (found == m_keys.end()) {
      return false;
    }
    end(found->second, timeMsec, events);
    m_keys.erase(found);
    return true;
  }

  void ShortcutKeyTracker::suspend(uint32_t timeMsec, std::vector<ShortcutEvent>& events) {
    for (auto& [key, entry] : m_keys) {
      end(entry, timeMsec, events);
    }
  }

  void ShortcutKeyTracker::reconcile(const ShortcutRegistry& registry, uint32_t timeMsec, std::vector<ShortcutEvent>& events) {
    for (auto& [key, entry] : m_keys) {
      if (entry.phase == KeyPhase::Held && !registry.live(entry.binding)) {
        end(entry, timeMsec, events);
      }
    }
  }

  void ShortcutKeyTracker::removeKeyboard(Keyboard keyboard, uint32_t timeMsec, std::vector<ShortcutEvent>& events) {
    std::erase_if(m_keys, [&](auto& entry) {
      if (entry.first.keyboard != keyboard) {
        return false;
      }
      end(entry.second, timeMsec, events);
      return true;
    });
  }

  bool ShortcutKeyTracker::holding() const {
    return std::ranges::any_of(m_keys, [](const auto& entry) { return entry.second.phase == KeyPhase::Held; });
  }

} // namespace andromeda
