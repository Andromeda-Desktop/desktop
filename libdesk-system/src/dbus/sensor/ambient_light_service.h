#pragma once

#include "core/timer_manager.h"

#include <chrono>
#include <functional>
#include <memory>

namespace sdbus {
  class IProxy;
}
class SystemBus;

// Ambient light in lux and colour temperature in kelvin (0 without a colour channel) from System1 (org.andromeda.System1.Sensors.ReadLight), sampled every 3 s, backing off to 15 s
// while the light stays steady.
class AmbientLightService {
public:
  using ReadingCallback = std::function<void(double lux, int kelvin)>;

  AmbientLightService(SystemBus& bus, ReadingCallback onReading);
  ~AmbientLightService();

  AmbientLightService(const AmbientLightService&) = delete;
  AmbientLightService& operator=(const AmbientLightService&) = delete;

  // Paused: no sampling; resuming samples at once.
  void setPaused(bool paused);

private:
  void sample();
  void onReading(double lux, int kelvin);

  std::unique_ptr<sdbus::IProxy> m_proxy;
  ReadingCallback m_onReading;
  Timer m_nextSample;
  bool m_paused = false;
  bool m_readFailed = false;
  double m_lastLux = -1.0;
  std::chrono::seconds m_interval{3};
  std::shared_ptr<int> m_lifetimeToken = std::make_shared<int>(0);
};
