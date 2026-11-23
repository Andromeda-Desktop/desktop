#include "qos/on_battery_watch.h"

#include "core/log.h"

#include <cstring>

#include <systemd/sd-bus.h>

namespace andromeda::qos {

  namespace {

    constexpr Logger kLog("qos");
    constexpr const char* kName = "org.andromeda.System1";
    constexpr const char* kPath = "/org/andromeda/System1/Power";
    constexpr const char* kInterface = "org.andromeda.System1.Power";

    // Reads a "b" variant into `out`; false on any other type.
    bool readBoolVariant(sd_bus_message* message, bool& out) {
      int value = 0;
      if (sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, "b") <= 0
          || sd_bus_message_read(message, "b", &value) < 0) {
        return false;
      }
      sd_bus_message_exit_container(message);
      out = value != 0;
      return true;
    }

    // "power-saver", "balanced" or "performance"; anything else (empty: no profile backend) counts as Balanced.
    Profile parseProfile(const char* name) {
      if (std::strcmp(name, "power-saver") == 0) {
        return Profile::PowerSaver;
      }
      return std::strcmp(name, "performance") == 0 ? Profile::Performance : Profile::Balanced;
    }

  } // namespace

  OnBatteryWatch::OnBatteryWatch(wl_event_loop* loop, std::function<void()> changed)
      : m_changed(std::move(changed)), m_bus(loop, bus::Kind::System) {
    m_bus.onChange([this](sd_bus* bus) {
      if (bus != nullptr) {
        follow();
        return;
      }
      m_getSlot = sd_bus_slot_unref(m_getSlot);
      set(std::nullopt, Profile::Balanced);
    });
    if (m_bus.get() != nullptr) {
      follow();
    }
  }

  OnBatteryWatch::~OnBatteryWatch() { m_getSlot = sd_bus_slot_unref(m_getSlot); }

  void OnBatteryWatch::follow() {
    if (sd_bus_match_signal_async(
            m_bus.get(), m_bus.keep(), kName, kPath, "org.freedesktop.DBus.Properties", "PropertiesChanged",
            onPropertiesChanged, nullptr, this
        ) < 0
        || sd_bus_add_match_async(
               m_bus.get(), m_bus.keep(),
               "type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.andromeda.System1'",
               onOwnerChanged, nullptr, this
           ) < 0) {
      kLog.warn("cannot follow System1 power: on-battery and profile stay unknown");
      return;
    }
    requestValue();
  }

  void OnBatteryWatch::requestValue() {
    m_getSlot = sd_bus_slot_unref(m_getSlot);
    if (sd_bus_call_method_async(
            m_bus.get(), &m_getSlot, kName, kPath, "org.freedesktop.DBus.Properties", "GetAll", onGetReply, this, "s",
            kInterface
        )
        < 0) {
      m_getSlot = nullptr;
    }
    m_bus.sent();
  }

  void OnBatteryWatch::set(std::optional<bool> onBattery, Profile profile) {
    if (onBattery == m_onBattery && profile == m_profile) {
      return;
    }
    if (onBattery != m_onBattery) {
      kLog.info(
          "power source: {}", !onBattery ? "unknown (andromeda-system not on the bus)" : *onBattery ? "battery" : "mains"
      );
    }
    if (profile != m_profile) {
      kLog.info(
          "power profile: {}", profile == Profile::PowerSaver ? "power-saver"
                               : profile == Profile::Performance ? "performance"
                                                                 : "balanced"
      );
    }
    m_onBattery = onBattery;
    m_profile = profile;
    if (m_changed) {
      m_changed();
    }
  }

  // Reads an a{sv} of Power properties; keeps the current value of each property the dict lacks.
  void OnBatteryWatch::readProperties(sd_bus_message* message) {
    if (sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sv}") <= 0) {
      return;
    }
    std::optional<bool> onBattery = m_onBattery;
    Profile profile = m_profile;
    while (sd_bus_message_enter_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv") > 0) {
      const char* name = nullptr;
      bool value = false;
      const char* text = nullptr;
      if (sd_bus_message_read(message, "s", &name) < 0) {
        return;
      }
      if (std::strcmp(name, "OnBattery") == 0 && readBoolVariant(message, value)) {
        onBattery = value;
      } else if (std::strcmp(name, "Profile") == 0 && sd_bus_message_read(message, "v", "s", &text) >= 0) {
        profile = parseProfile(text);
      } else if (sd_bus_message_skip(message, "v") < 0) {
        return;
      }
      sd_bus_message_exit_container(message);
    }
    set(onBattery, profile);
  }

  int OnBatteryWatch::onGetReply(sd_bus_message* reply, void* data, sd_bus_error* /*error*/) {
    auto* self = static_cast<OnBatteryWatch*>(data);
    self->m_getSlot = sd_bus_slot_unref(self->m_getSlot);
    if (sd_bus_message_is_method_error(reply, nullptr) == 0) {
      self->readProperties(reply);
    }
    return 0;
  }

  // PropertiesChanged: (s interface, a{sv} changed, as invalidated).
  int OnBatteryWatch::onPropertiesChanged(sd_bus_message* message, void* data, sd_bus_error* /*error*/) {
    auto* self = static_cast<OnBatteryWatch*>(data);
    const char* interface = nullptr;
    if (sd_bus_message_read(message, "s", &interface) >= 0 && std::strcmp(interface, kInterface) == 0) {
      self->readProperties(message);
    }
    return 0;
  }

  // NameOwnerChanged: (s name, s old owner, s new owner). A restarted daemon may report a new value.
  int OnBatteryWatch::onOwnerChanged(sd_bus_message* message, void* data, sd_bus_error* /*error*/) {
    auto* self = static_cast<OnBatteryWatch*>(data);
    const char* name = nullptr;
    const char* oldOwner = nullptr;
    const char* newOwner = nullptr;
    if (sd_bus_message_read(message, "sss", &name, &oldOwner, &newOwner) < 0 || std::strcmp(name, kName) != 0) {
      return 0;
    }
    if (*newOwner == '\0') {
      self->set(std::nullopt, Profile::Balanced);
    } else {
      self->requestValue();
    }
    return 0;
  }

} // namespace andromeda::qos
