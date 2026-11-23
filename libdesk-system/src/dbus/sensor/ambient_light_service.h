#pragma once

#include "core/timer_manager.h"

#include <chrono>
#include <functional>
#include <memory>

namespace sdbus {
  class IProxy;
}
class SystemBus;

// Ambient light in lux from System1 (org.andromeda.System1.Sensors.ReadLight), sampled every 10 s, backing off to 60 s
// while the light stays steady.
class AmbientLightService {
public:
  using LuxCallback = std::function<void(double lux)>;

  AmbientLightService(SystemBus& bus, LuxCallback onLux);
  ~AmbientLightService();

  AmbientLightService(const AmbientLightService&) = delete;
  AmbientLightService& operator=(const AmbientLightService&) = delete;

  // Paused: no sampling; resuming samples at once.
  void setPaused(bool paused);

private:
  void sample();
  void onReading(double lux);

  std::unique_ptr<sdbus::IProxy> m_proxy;
  LuxCallback m_onLux;
  Timer m_nextSample;
  bool m_paused = false;
  bool m_readFailed = false;
  double m_lastLux = -1.0;
  std::chrono::seconds m_interval{10};
  std::shared_ptr<int> m_lifetimeToken = std::make_shared<int>(0);
};
