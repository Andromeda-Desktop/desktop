#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace sdbus {
  class IProxy;
}
class SystemBus;

enum class Presence : std::uint8_t { Unknown = 0, Absent = 1, Present = 2 };

// Holds a System1 presence claim (org.andromeda.System1.Sensors.ClaimPresence) for its lifetime; sensing runs only
// while some client holds one. Reports each change of the Presence property.
class PresenceService {
public:
  using ChangeCallback = std::function<void(Presence)>;

  PresenceService(SystemBus& bus, ChangeCallback onChange);
  ~PresenceService();

  PresenceService(const PresenceService&) = delete;
  PresenceService& operator=(const PresenceService&) = delete;

private:
  void report(std::uint32_t value);

  std::unique_ptr<sdbus::IProxy> m_proxy;
  ChangeCallback m_onChange;
  std::shared_ptr<int> m_lifetimeToken = std::make_shared<int>(0);
};
