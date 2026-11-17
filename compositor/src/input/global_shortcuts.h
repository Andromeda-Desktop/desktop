#pragma once

#include <xkbcommon/xkbcommon.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace andromeda {

  // Modifier bits, equal to WLR_MODIFIER_* so a keyboard's mask compares directly.
  namespace shortcut_mod {
    inline constexpr uint32_t kShift = 1U << 0;
    inline constexpr uint32_t kCtrl = 1U << 2;
    inline constexpr uint32_t kAlt = 1U << 3;
    inline constexpr uint32_t kLogo = 1U << 6;
    inline constexpr uint32_t kAll = kShift | kCtrl | kAlt | kLogo;
  } // namespace shortcut_mod

  // A key chord: modifier bits and a lowercase keysym.
  struct ShortcutChord {
    uint32_t modifiers = 0;
    xkb_keysym_t keysym = XKB_KEY_NoSymbol;
    bool operator==(const ShortcutChord&) const = default;
    auto operator<=>(const ShortcutChord&) const = default;
  };

  // Parses an XDG shortcuts trigger such as "CTRL+ALT+m". Any malformed token rejects the whole trigger.
  [[nodiscard]] std::optional<ShortcutChord> parsePortalTrigger(std::string_view text);
  // "Ctrl+Alt+Shift+Super+M".
  [[nodiscard]] std::string describeChord(const ShortcutChord& chord);
  // An app may only suggest chords that cannot steal typing: one with Ctrl, Alt or Super, or a non-typing key.
  [[nodiscard]] bool preferredChordAllowed(const ShortcutChord& chord);

  using ShortcutSessionId = uint64_t;

  struct ShortcutRequest {
    std::string id;
    std::string description;
    std::optional<ShortcutChord> preferred;
  };

  enum class ShortcutSource : uint8_t { None, Config, Stored };
  enum class ShortcutConflict : uint8_t { None, Unassigned, Disabled, Reserved, InUse };

  struct ShortcutState {
    std::string id;
    std::string description;
    std::optional<ShortcutChord> chord;
    ShortcutSource source = ShortcutSource::None;
    ShortcutConflict conflict = ShortcutConflict::Unassigned;
    [[nodiscard]] bool live() const { return conflict == ShortcutConflict::None; }
  };

  // The approved chords of one app, by shortcut id. A shortcut approved without a usable chord has no entry.
  using ShortcutGrants = std::map<std::string, std::map<std::string, ShortcutChord>>;
  // User overrides by app and shortcut id; nullopt disables the shortcut.
  using ShortcutOverrides = std::map<std::string, std::map<std::string, std::optional<ShortcutChord>>>;

  // Sessions, grants and the live chord table. No I/O: the caller persists grants() and sends events.
  class ShortcutRegistry {
  public:
    enum class Phase : uint8_t { Unbound, Approving, Bound, Closed };
    enum class BindResult : uint8_t { Bound, NeedsApproval, Rejected };

    explicit ShortcutRegistry(std::function<bool(const ShortcutChord&)> reserved = {});

    ShortcutSessionId open(std::string app);
    // Unbound or Bound -> Bound when every shortcut already has a grant or override, else -> Approving (queued).
    BindResult bind(ShortcutSessionId session, std::vector<ShortcutRequest> shortcuts);
    // Approving -> Bound for the session at the head of the approval queue. Grants its usable preferred chords.
    bool approve(ShortcutSessionId session);
    // Approving -> Unbound.
    bool cancel(ShortcutSessionId session);
    void close(ShortcutSessionId session);
    // Drops the app's grants; its bound sessions return to Unbound. An empty app revokes every app.
    void revoke(std::string_view app);

    void setGrants(ShortcutGrants grants);
    void setOverrides(ShortcutOverrides overrides);
    void setReserved(std::function<bool(const ShortcutChord&)> reserved);

    [[nodiscard]] const ShortcutGrants& grants() const { return m_grants; }
    [[nodiscard]] Phase phase(ShortcutSessionId session) const;
    [[nodiscard]] std::optional<ShortcutSessionId> pendingApproval() const;
    [[nodiscard]] const std::string* app(ShortcutSessionId session) const;
    [[nodiscard]] std::vector<ShortcutState> shortcuts(ShortcutSessionId session) const;
    [[nodiscard]] std::vector<ShortcutSessionId> sessions() const;

    struct Binding {
      ShortcutSessionId session = 0;
      std::string id;
      bool operator==(const Binding&) const = default;
    };
    [[nodiscard]] const Binding* match(const ShortcutChord& chord) const;
    [[nodiscard]] bool live(const Binding& binding) const;
    // Bumps on every change to the live table or to a bound session's shortcut states.
    [[nodiscard]] uint64_t generation() const { return m_generation; }

  private:
    struct Session {
      std::string app;
      std::vector<ShortcutRequest> shortcuts;
      Phase phase = Phase::Unbound;
      uint64_t boundOrder = 0;
      std::vector<ShortcutState> states;
    };

    bool transition(Session& session, Phase next);
    [[nodiscard]] bool covered(const Session& session) const;
    void resolve();
    [[nodiscard]] bool chordTaken(const ShortcutChord& chord) const;

    std::function<bool(const ShortcutChord&)> m_reserved;
    std::map<ShortcutSessionId, Session> m_sessions;
    std::deque<ShortcutSessionId> m_approvals;
    ShortcutGrants m_grants;
    ShortcutOverrides m_overrides;
    std::map<ShortcutChord, Binding> m_live;
    ShortcutSessionId m_nextSession = 1;
    uint64_t m_nextOrder = 1;
    uint64_t m_generation = 0;
  };

  struct ShortcutEvent {
    enum class Kind : uint8_t { Activated, Deactivated };
    Kind kind = Kind::Activated;
    ShortcutSessionId session = 0;
    std::string id;
    uint32_t timeMsec = 0;
    bool operator==(const ShortcutEvent&) const = default;
  };

  // Per physical key: which binding it holds. Each Activated it emits is followed by exactly one Deactivated, and a
  // key whose press it consumed keeps consuming its repeats and its release.
  class ShortcutKeyTracker {
  public:
    using Keyboard = uintptr_t;

    // True when the press belongs to a global shortcut and must not reach clients.
    bool press(
        Keyboard keyboard, uint32_t keycode, const ShortcutChord& chord, bool allowed, const ShortcutRegistry& registry,
        uint32_t timeMsec, std::vector<ShortcutEvent>& events
    );
    bool release(Keyboard keyboard, uint32_t keycode, uint32_t timeMsec, std::vector<ShortcutEvent>& events);
    // Ends every held binding (lock, inhibit, capture, approval dialog); their releases stay consumed.
    void suspend(uint32_t timeMsec, std::vector<ShortcutEvent>& events);
    // Ends held bindings the registry no longer has live.
    void reconcile(const ShortcutRegistry& registry, uint32_t timeMsec, std::vector<ShortcutEvent>& events);
    void removeKeyboard(Keyboard keyboard, uint32_t timeMsec, std::vector<ShortcutEvent>& events);
    [[nodiscard]] bool holding() const;

  private:
    struct Key {
      Keyboard keyboard = 0;
      uint32_t keycode = 0;
      bool operator==(const Key&) const = default;
    };
    struct KeyHash {
      size_t operator()(const Key& key) const noexcept {
        return std::hash<uint64_t>{}((static_cast<uint64_t>(key.keyboard) << 9U) ^ key.keycode);
      }
    };
    // Held: the binding is active. Swallowed: the binding ended early; the release is still consumed.
    enum class KeyPhase : uint8_t { Held, Swallowed };
    struct Entry {
      KeyPhase phase = KeyPhase::Held;
      ShortcutRegistry::Binding binding;
    };

    static void end(Entry& entry, uint32_t timeMsec, std::vector<ShortcutEvent>& events);

    std::unordered_map<Key, Entry, KeyHash> m_keys;
  };

} // namespace andromeda
