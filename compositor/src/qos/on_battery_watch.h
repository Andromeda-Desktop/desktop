#pragma once

#include "core/loop_bus.h"
#include "qos/policy.h"

#include <cstdint>
#include <functional>
#include <optional>

struct sd_bus;
struct sd_bus_error;
struct sd_bus_message;
struct sd_bus_slot;
struct wl_event_loop;
struct wl_event_source;

namespace andromeda::qos {

  // Follows org.andromeda.System1.Power OnBattery and Profile over the system bus without blocking the event loop. Reading
  // /sys/class/power_supply/BAT*/status runs ACPI _BST through the EC and can take 40 ms on the main thread.
  class OnBatteryWatch {
  public:
    // `changed` runs after the value changes, never from inside the constructor.
    OnBatteryWatch(wl_event_loop* loop, std::function<void()> changed);
    ~OnBatteryWatch();

    OnBatteryWatch(const OnBatteryWatch&) = delete;
    OnBatteryWatch& operator=(const OnBatteryWatch&) = delete;

    // Empty until andromeda-system answered, and while it is not on the bus.
    [[nodiscard]] std::optional<bool> onBattery() const { return m_onBattery; }
    // Balanced until andromeda-system answered, and while it is not on the bus or runs no profile backend.
    [[nodiscard]] Profile profile() const { return m_profile; }

  private:
    static int onGetReply(sd_bus_message* reply, void* data, sd_bus_error* error);
    static int onPropertiesChanged(sd_bus_message* message, void* data, sd_bus_error* error);
    static int onOwnerChanged(sd_bus_message* message, void* data, sd_bus_error* error);
    void follow();
    void requestValue();
    void set(std::optional<bool> onBattery, Profile profile);
    void readProperties(sd_bus_message* message);

    std::function<void()> m_changed;
    bus::Client m_bus;
    sd_bus_slot* m_getSlot = nullptr;
    std::optional<bool> m_onBattery;
    Profile m_profile = Profile::Balanced;
  };

} // namespace andromeda::qos
