#include "dbus/sensor/ambient_light_service.h"
#include "protocols/bus_names.h"

#include "core/log.h"
#include "dbus/system_bus.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>

namespace {
  constexpr Logger kLog("ambient-light");
  const sdbus::ServiceName kBusName{busname::kSystem1};
  const sdbus::ObjectPath kPath{"/org/andromeda/System1/Sensors"};
  constexpr auto kInterface = "org.andromeda.System1.Sensors";
  constexpr auto kSampleInterval = std::chrono::seconds(3);
  constexpr auto kMaxSampleInterval = std::chrono::seconds(15);
} // namespace

AmbientLightService::AmbientLightService(SystemBus& bus, ReadingCallback onReading)
    : m_onReading(std::move(onReading)) {
  m_proxy = sdbus::createProxy(bus.connection(), kBusName, kPath);
  sample();
}

AmbientLightService::~AmbientLightService() { m_lifetimeToken.reset(); }

void AmbientLightService::sample() {
  const std::weak_ptr<int> alive = m_lifetimeToken;
  m_proxy->callMethodAsync("ReadLight")
      .onInterface(kInterface)
      .uponReplyInvoke([this, alive](std::optional<sdbus::Error> err, double lux, std::uint32_t kelvin) {
        if (alive.expired() || m_paused) {
          return;
        }
        // No sensor: stop until the next resume (setPaused(false)); System1 restarting: retry at the slow interval.
        if (err.has_value()) {
          if (!m_readFailed) {
            kLog.info("no ambient light sensor: {}", err->what());
          }
          m_readFailed = true;
          if (err->getName() != "org.freedesktop.DBus.Error.NotSupported") {
            m_nextSample.start(kMaxSampleInterval, [this] { sample(); });
          }
          return;
        }
        m_readFailed = false;
        onReading(lux, static_cast<int>(std::min<std::uint32_t>(kelvin, 100000)));
        m_nextSample.start(m_interval, [this] { sample(); });
      });
}

// A steady reading (within 15 % or 5 lux) doubles the interval up to 15 s; a change returns it to 3 s.
void AmbientLightService::onReading(double lux, int kelvin) {
  const bool steady = m_lastLux >= 0.0 && std::abs(lux - m_lastLux) <= std::max(5.0, 0.15 * m_lastLux);
  m_interval = steady ? std::min(m_interval * 2, kMaxSampleInterval) : kSampleInterval;
  m_lastLux = lux;
  m_onReading(lux, kelvin);
}

void AmbientLightService::setPaused(bool paused) {
  if (paused == m_paused) {
    return;
  }
  m_paused = paused;
  m_nextSample.stop();
  if (!paused) {
    m_interval = kSampleInterval;
    sample();
  }
}

SensorAvailability::SensorAvailability(SystemBus& bus, std::function<void()> onChanged) {
  m_proxy = sdbus::createProxy(bus.connection(), kBusName, kPath);
  const std::weak_ptr<int> alive = m_lifetimeToken;
  m_proxy->getAllPropertiesAsync()
      .onInterface(kInterface)
      .uponReplyInvoke([this, alive, onChanged = std::move(onChanged)](
                           std::optional<sdbus::Error> err, std::map<sdbus::PropertyName, sdbus::Variant> props) {
        if (alive.expired() || err.has_value()) {
          return;
        }
        // A System1 older than the property has the sensor calls but cannot say: keep the rows.
        const auto flag = [&props](const char* name) {
          const auto it = props.find(sdbus::PropertyName{name});
          return it == props.end() || !it->second.containsValueOfType<bool>() || it->second.get<bool>();
        };
        m_light = flag("HasLight");
        m_presence = flag("HasPresence");
        if (m_light || m_presence) {
          onChanged();
        }
      });
}

SensorAvailability::~SensorAvailability() { m_lifetimeToken.reset(); }
