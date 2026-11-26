#pragma once

#include "dbus/name_owner_watch.h"

#include "core/latest_request.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class SystemBus;

namespace sdbus {
  class IProxy;
}

[[nodiscard]] std::string profileLabel(std::string_view profile);
[[nodiscard]] std::string_view profileGlyphName(std::string_view profile);

/// Canonical low-power to high-power ordering of the well-known profiles.
[[nodiscard]] std::span<const std::string_view> powerProfileOrder();

struct PowerProfilesState {
  std::string activeProfile;
  std::vector<std::string> profiles;
  std::string performanceInhibited;

  bool operator==(const PowerProfilesState&) const = default;
};

enum class PowerProfilesChangeOrigin : std::uint8_t {
  External,
  Noctalia,
};

class PowerProfilesService {
public:
  using ChangeCallback = std::function<void(const PowerProfilesState&, PowerProfilesChangeOrigin)>;

  explicit PowerProfilesService(SystemBus& bus);
  ~PowerProfilesService();

  void setChangeCallback(ChangeCallback callback);
  void refresh();

  [[nodiscard]] bool hasStateSnapshot() const noexcept { return m_hasStateSnapshot; }
  [[nodiscard]] const PowerProfilesState& state() const noexcept { return m_state; }
  [[nodiscard]] const std::string& activeProfile() const noexcept { return m_state.activeProfile; }
  [[nodiscard]] const std::vector<std::string>& profiles() const noexcept { return m_state.profiles; }

  [[nodiscard]] bool setActiveProfile(std::string_view profile);
  /// An automatic System1 hold on power.profile (empty releases it): rule holds merge with it lowest power first, and
  /// the user's next profile choice ends it. Not a user choice, so it never undoes a rule episode.
  void holdProfile(std::string profile, std::string reason);
  /// The profile of the last change the daemon refused; empty after the next change starts.
  [[nodiscard]] const std::string& failedProfile() const noexcept { return m_failedProfile; }
  /// Step through profiles in canonical power order, wrapping at the ends.
  /// direction >= 0 moves toward performance, direction < 0 toward power-saver.
  /// Returns false when no active profile is known and UPower returned no profile list,
  /// or when the D-Bus set dispatch fails synchronously.
  [[nodiscard]] bool cycleActiveProfile(int direction = 1);

private:
  [[nodiscard]] PowerProfilesChangeOrigin consumeActiveProfileChangeOrigin(std::string_view profile);
  void reportFailure(const std::string& profile);
  void emitChangedIfNeeded(PowerProfilesState next, bool stateSnapshot);
  void sendHold();

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_proxy;
  std::shared_ptr<int> m_lifetimeToken = std::make_shared<int>(0);
  PowerProfilesState m_state;
  std::optional<std::string> m_pendingLocalActiveProfile;
  std::string m_failedProfile;
  ChangeCallback m_changeCallback;
  struct ProfileHold {
    std::string profile;
    std::string reason;
    // Unset while System1 has not answered the Hold, or after System1 left the bus (the hold died with it).
    std::optional<std::string> id;
  };
  // The holdProfile request, kept until it is released or a user choice ends it, so a restarted System1 gets it again.
  std::optional<ProfileHold> m_hold;
  LatestRequest m_holdRequest;
  bool m_hasStateSnapshot = false;
  bool m_refreshInFlight = false;
  bool m_refreshQueued = false;
  std::unique_ptr<sdbus::IProxy> m_knobsProxy;
  NameOwnerWatch m_ownerWatch;
};
