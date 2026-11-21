#include "qos/policy.h"

namespace andromeda::qos {

  Decision ScopePolicy::update(const AppInputs& inputs, Power power, std::uint64_t nowMs) {
    if (inputs.focused || inputs.visible) {
      m_hiddenSinceMs.reset();
      return {.tier = inputs.focused ? Tier::Focus : Tier::Visible};
    }
    if (!m_hiddenSinceMs) {
      m_hiddenSinceMs = nowMs;
    }
    if (exempt(inputs)) {
      return {.tier = Tier::Visible};
    }
    const bool saver = power.profile == Profile::PowerSaver;
    const bool balancedBattery = power.profile == Profile::Balanced && power.onBattery;
    const bool ruled = inputs.rule == Rule::FreezeWhenHidden;
    // A profile or battery change is an input change and calls update again.
    if (!saver && !balancedBattery && !ruled) {
      return {.tier = Tier::Visible};
    }
    const std::uint64_t hiddenAt = *m_hiddenSinceMs + (balancedBattery && !ruled ? m_delays.balancedHiddenMs : m_delays.hiddenMs);
    if (nowMs < hiddenAt) {
      return {.tier = Tier::Visible, .recheckAtMs = hiddenAt};
    }
    const bool longComes = saver || power.onBattery;
    const std::uint64_t longAt = hiddenAt + m_delays.hiddenLongMs;
    if (!longComes || nowMs < longAt) {
      return {.tier = Tier::Hidden, .recheckAtMs = longComes ? std::optional(longAt) : std::nullopt};
    }
    const bool freezes = ruled || inputs.freezeHidden || (inputs.background && !inputs.trayItem && !inputs.busService);
    return {.tier = Tier::HiddenLong, .frozen = freezes && !inputs.thawIntent};
  }

} // namespace andromeda::qos
