#include "qos/policy.h"

#include "check.h"

#include <array>
#include <optional>

using andromeda::qos::AppInputs;
using andromeda::qos::Decision;
using andromeda::qos::kBalancedHiddenDelayMs;
using andromeda::qos::kHiddenDelayMs;
using andromeda::qos::kHiddenLongDelayMs;
using andromeda::qos::Power;
using andromeda::qos::Profile;
using andromeda::qos::Rule;
using andromeda::qos::ScopePolicy;
using andromeda::qos::Tier;

namespace {
  constexpr std::uint64_t kStart = 1'000'000;

  // The docs/qos.md §3 table, written as a lookup: the tier an app has `elapsed` ms after it was hidden.
  Decision expected(const AppInputs& in, Power power, std::uint64_t elapsed) {
    if (in.focused) {
      return {.tier = Tier::Focus};
    }
    if (in.visible || in.audio || in.call || in.idleInhibitor || in.screenCast || in.userWork
        || in.rule == Rule::NeverThrottle) {
      return {.tier = Tier::Visible};
    }
    const bool ruled = in.rule == Rule::FreezeWhenHidden;
    std::optional<std::uint64_t> delay;
    if (power.profile == Profile::PowerSaver || ruled) {
      delay = kHiddenDelayMs;
    } else if (power.profile == Profile::Balanced && power.onBattery) {
      delay = kBalancedHiddenDelayMs;
    }
    if (!delay || elapsed < *delay) {
      return {.tier = Tier::Visible};
    }
    const bool longComes = power.profile == Profile::PowerSaver || power.onBattery;
    if (!longComes || elapsed < *delay + kHiddenLongDelayMs) {
      return {.tier = Tier::Hidden};
    }
    const bool freezes = ruled || in.freezeHidden || (in.background && !in.trayItem && !in.busService);
    return {.tier = Tier::HiddenLong, .frozen = freezes && !in.thawIntent};
  }

  constexpr std::array kElapsed{
      std::uint64_t{0},
      kHiddenDelayMs - 1,
      kHiddenDelayMs,
      kBalancedHiddenDelayMs - 1,
      kBalancedHiddenDelayMs,
      kHiddenDelayMs + kHiddenLongDelayMs - 1,
      kHiddenDelayMs + kHiddenLongDelayMs,
      kBalancedHiddenDelayMs + kHiddenLongDelayMs - 1,
      kBalancedHiddenDelayMs + kHiddenLongDelayMs,
      std::uint64_t{24} * 60 * 60 * 1000,
  };

  // Every combination of the inputs: 16 flags/choices, about 200k states over 6 power states.
  template <typename F> void forEachInputs(F&& f) {
    for (unsigned bits = 0; bits < (1U << 12); ++bits) {
      for (const Rule rule : {Rule::Normal, Rule::NeverThrottle, Rule::FreezeWhenHidden}) {
        const auto bit = [bits](unsigned i) { return (bits >> i & 1U) != 0; };
        f(AppInputs{
            .focused = bit(0), .visible = bit(1), .audio = bit(2), .call = bit(3), .idleInhibitor = bit(4),
            .screenCast = bit(5), .userWork = bit(6), .background = bit(7), .trayItem = bit(8), .busService = bit(11), .thawIntent = bit(9),
            .freezeHidden = bit(10), .rule = rule,
        });
      }
    }
  }

  template <typename F> void forEachPower(F&& f) {
    for (const Profile profile : {Profile::PowerSaver, Profile::Balanced, Profile::Performance}) {
      for (const bool battery : {false, true}) {
        f(Power{.profile = profile, .onBattery = battery});
      }
    }
  }
} // namespace

ANDROMEDA_TEST(everyReachableStateMatchesTheTierTableAndKeepsTheInvariants) {
  forEachPower([](Power power) {
    forEachInputs([power](const AppInputs& in) {
      ScopePolicy policy;
      Tier last = Tier::Focus;
      for (const std::uint64_t elapsed : kElapsed) {
        const Decision got = policy.update(in, power, kStart + elapsed);
        const Decision want = expected(in, power, elapsed);
        CHECK(got.tier == want.tier);
        CHECK(got.frozen == want.frozen);
        // I2: an exempt app never drops below Visible and never freezes.
        CHECK(!andromeda::qos::exempt(in) || (got.tier <= Tier::Visible && !got.frozen));
        // I3, I4: Performance, and Balanced on mains, throttle only a freeze_when_hidden app.
        const bool calm = power.profile == Profile::Performance || (power.profile == Profile::Balanced && !power.onBattery);
        CHECK(!calm || in.rule == Rule::FreezeWhenHidden || (got.tier <= Tier::Visible && !got.frozen));
        // I5: only T3 freezes, and intent always thaws.
        CHECK(!got.frozen || (got.tier == Tier::HiddenLong && !in.thawIntent));
        // I6: for fixed inputs the tier never rises back while time passes.
        CHECK(got.tier >= last || elapsed == 0);
        last = got.tier;
        // A recheck is due exactly when the tier can still drop with no input change.
        CHECK(got.recheckAtMs.has_value() == (expected(in, power, std::uint64_t{24} * 60 * 60 * 1000).tier != got.tier));
      }
    });
  });
}

ANDROMEDA_TEST(anInputChangePromotesAtOnceAndTheHiddenClockKeepsRunning) {
  forEachPower([](Power power) {
    ScopePolicy policy;
    const AppInputs hidden{};
    const std::uint64_t late = kStart + kBalancedHiddenDelayMs + kHiddenLongDelayMs;
    policy.update(hidden, power, kStart);
    policy.update(hidden, power, late);
    CHECK(policy.update({.visible = true}, power, late + 1).tier == Tier::Visible);
    CHECK(policy.update({.audio = true}, power, late + 2).tier == Tier::Visible);
    // Hidden again: the clock starts over.
    policy.update({.visible = true}, power, late + 3);
    CHECK(policy.update(hidden, power, late + 4).tier == Tier::Visible);
  });
  // A profile change acts on the time already hidden: Balanced on mains to Power Saver after 10 min is T3 at once.
  ScopePolicy policy;
  policy.update({}, Power{}, kStart);
  CHECK(policy.update({}, Power{}, kStart + 600'000).tier == Tier::Visible);
  CHECK(policy.update({}, Power{.profile = Profile::PowerSaver}, kStart + 600'001).tier == Tier::HiddenLong);
}

int main() { return RUN_TESTS(); }
