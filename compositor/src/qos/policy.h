#pragma once

#include <cstdint>
#include <optional>

// App quality of service policy (docs/qos.md §3). Pure: no Wayland, D-Bus or clock; the caller passes the time.
namespace andromeda::qos {

  enum class Tier : std::uint8_t { Focus, Visible, Hidden, HiddenLong };

  // Window rule `qos`.
  enum class Rule : std::uint8_t { Normal, NeverThrottle, FreezeWhenHidden };

  // The power profile (org.andromeda.System1.Power Profile); Balanced while no profile backend runs.
  enum class Profile : std::uint8_t { PowerSaver, Balanced, Performance };

  struct Power {
    Profile profile = Profile::Balanced;
    bool onBattery = false;
    bool operator==(const Power&) const = default;
  };

  // One app scope at one moment. With several windows, focused/visible hold when any window is.
  struct AppInputs {
    bool focused = false;
    bool visible = false; // some window is not suspended
    bool audio = false;
    bool call = false;
    bool idleInhibitor = false;
    bool screenCast = false;
    bool userWork = false; // holds a sleep inhibitor: a copy, an export or a download the user started
    bool background = false; // an app unit with no mapped window, with general.freeze_background_apps on
    bool trayItem = false; // owns a tray item: the user reaches it there, so it never freezes
    bool thawIntent = false; // the user starts to reach the app (switcher, dock hover, workspace swipe)
    bool freezeHidden = false; // power saver on battery with general.freeze_hidden_apps: T3 freezes every app
    Rule rule = Rule::Normal;
  };

  struct Decision {
    Tier tier = Tier::Visible;
    bool frozen = false;
    // When the tier can drop with no input change; nullopt = only an input change moves it.
    std::optional<std::uint64_t> recheckAtMs{};
  };

  inline constexpr std::uint64_t kHiddenDelayMs = 5'000; // Power Saver, and a freeze_when_hidden rule
  inline constexpr std::uint64_t kBalancedHiddenDelayMs = 30'000; // Balanced on battery: a quick switch keeps all cores
  inline constexpr std::uint64_t kHiddenLongDelayMs = 3ULL * 60 * 1000; // in T2 before T3

  struct Delays {
    std::uint64_t hiddenMs = kHiddenDelayMs;
    std::uint64_t balancedHiddenMs = kBalancedHiddenDelayMs;
    std::uint64_t hiddenLongMs = kHiddenLongDelayMs;
  };

  // An app the user still uses while it is hidden: it keeps the visible tier, ignores automatic holds, never freezes.
  [[nodiscard]] constexpr bool exempt(const AppInputs& inputs) {
    return inputs.audio || inputs.call || inputs.idleInhibitor || inputs.screenCast || inputs.userWork
        || inputs.rule == Rule::NeverThrottle;
  }

  // A held thaw intent (a workspace slide, the app switcher) reaches a scope only through a window that motion can
  // show: windowless scopes and windows on other workspaces stay frozen.
  [[nodiscard]] constexpr bool thawIntent(bool held, bool windowShown) { return held && windowShown; }

  class ScopePolicy {
  public:
    explicit ScopePolicy(Delays delays = {}) : m_delays(delays) {}
    // Promotes at once; demotes only after the delays, counted from the moment every window was hidden. Performance
    // and Balanced on mains throttle only a freeze_when_hidden app; Power Saver also takes T3 on mains (docs/qos.md §3).
    Decision update(const AppInputs& inputs, Power power, std::uint64_t nowMs);

  private:
    Delays m_delays;
    std::optional<std::uint64_t> m_hiddenSinceMs;
  };

} // namespace andromeda::qos
