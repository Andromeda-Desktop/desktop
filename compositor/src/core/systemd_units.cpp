#include "core/systemd_units.h"

#include "core/log.h"


#include <cstddef>
#include <cstdint>
#include <utility>

#include <systemd/sd-bus.h>

namespace andromeda {

  namespace {
    constexpr Logger kLog("systemd-units");
    constexpr const char* kService = "org.freedesktop.systemd1";
    constexpr const char* kPath = "/org/freedesktop/systemd1";
    constexpr const char* kManager = "org.freedesktop.systemd1.Manager";

    // The first failure is a warning (a missing cgroup delegation fails every call); the rest are debug lines.
    void logFailure(const std::string& what, sd_bus_message* reply) {
      static bool warned = false;
      const sd_bus_error* error = sd_bus_message_get_error(reply);
      const char* message = error != nullptr && error->message != nullptr ? error->message : "?";
      if (!std::exchange(warned, true)) {
        kLog.warn("{} failed: {}", what, message);
      } else {
        kLog.debug("{} failed: {}", what, message);
      }
    }

    // AllowedCPUs is a byte mask: bit n set = CPU n.
    std::vector<std::uint8_t> cpuMask(const std::vector<int>& cpus) {
      std::vector<std::uint8_t> mask;
      for (const int cpu : cpus) {
        const auto byte = static_cast<std::size_t>(cpu) / 8;
        if (mask.size() <= byte) {
          mask.resize(byte + 1, 0);
        }
        mask[byte] |= static_cast<std::uint8_t>(1U << (static_cast<unsigned>(cpu) % 8U));
      }
      return mask;
    }
  } // namespace

  SystemdUnits::SystemdUnits(wl_event_loop* loop) : m_bus(loop, bus::Kind::User) {
    if (m_bus.get() == nullptr) {
      kLog.info("no systemd user manager: app QoS and launch boost are off");
    }
  }

  SystemdUnits::~SystemdUnits() = default;

  // A reply never runs after the bus is gone.
  bool SystemdUnits::call(sd_bus_message* message, Reply reply) {
    return m_bus.call(message, [reply = std::move(reply)](sd_bus_message* answer) {
      if (answer != nullptr) {
        reply(answer);
      }
    });
  }

  void SystemdUnits::setProperties(const std::string& unit, const Properties& properties) {
    if (m_bus.get() == nullptr || unit.empty()) {
      return;
    }
    sd_bus_message* message = nullptr;
    bool built = sd_bus_message_new_method_call(m_bus.get(), &message, kService, kPath, kManager, "SetUnitProperties") >= 0
        && sd_bus_message_append(message, "sb", unit.c_str(), 1) >= 0
        && sd_bus_message_open_container(message, 'a', "(sv)") >= 0;
    if (built && properties.cpuWeight) {
      built = sd_bus_message_append(message, "(sv)", "CPUWeight", "t", *properties.cpuWeight) >= 0;
    }
    if (built && properties.ioWeight) {
      built = sd_bus_message_append(message, "(sv)", "IOWeight", "t", *properties.ioWeight) >= 0;
    }
    if (built && properties.memoryMin) {
      built = sd_bus_message_append(message, "(sv)", "MemoryMin", "t", *properties.memoryMin) >= 0;
    }
    if (built && properties.memoryLow) {
      built = sd_bus_message_append(message, "(sv)", "MemoryLow", "t", *properties.memoryLow) >= 0;
    }
    if (built && properties.allowedCpus) {
      const std::vector<std::uint8_t> mask = cpuMask(*properties.allowedCpus);
      static constexpr std::uint8_t kNone = 0;
      built = sd_bus_message_open_container(message, 'r', "sv") >= 0
          && sd_bus_message_append(message, "s", "AllowedCPUs") >= 0
          && sd_bus_message_open_container(message, 'v', "ay") >= 0
          && sd_bus_message_append_array(message, 'y', mask.empty() ? &kNone : mask.data(), mask.size()) >= 0
          && sd_bus_message_close_container(message) >= 0
          && sd_bus_message_close_container(message) >= 0;
    }
    if (built && sd_bus_message_close_container(message) >= 0) {
      call(message, [unit](sd_bus_message* reply) {
        if (sd_bus_message_is_method_error(reply, nullptr) != 0) {
          logFailure("SetUnitProperties " + unit, reply);
        }
      });
    }
    sd_bus_message_unref(message);
  }

  bool SystemdUnits::setFrozen(const std::string& unit, bool frozen, std::function<void(bool ok)> done) {
    if (m_bus.get() == nullptr || unit.empty()) {
      return false;
    }
    bool sent = false;
    const char* method = frozen ? "FreezeUnit" : "ThawUnit";
    sd_bus_message* message = nullptr;
    if (sd_bus_message_new_method_call(m_bus.get(), &message, kService, kPath, kManager, method) >= 0
        && sd_bus_message_append(message, "s", unit.c_str()) >= 0) {
      sent = call(message, [unit, frozen, method, done = std::move(done)](sd_bus_message* reply) {
        // A unit that exited or was unloaded has no cgroup left to thaw: the thaw has nothing to do.
        const bool gone = !frozen
            && (sd_bus_message_is_method_error(reply, "org.freedesktop.systemd1.UnitInactive") != 0
                || sd_bus_message_is_method_error(reply, "org.freedesktop.systemd1.NoSuchUnit") != 0);
        const bool ok = gone || sd_bus_message_is_method_error(reply, nullptr) == 0;
        if (!ok) {
          logFailure(std::string(method) + " " + unit, reply);
        }
        if (done) {
          done(ok);
        }
      });
    }
    sd_bus_message_unref(message);
    return sent;
  }

  bool SystemdUnits::startScope(
      const std::string& unit, int pid, std::function<void(bool ok)> done, const ScopeOptions& options
  ) {
    if (m_bus.get() == nullptr || unit.empty()) {
      return false;
    }
    sd_bus_message* message = nullptr;
    bool built = sd_bus_message_new_method_call(m_bus.get(), &message, kService, kPath, kManager, "StartTransientUnit") >= 0
        && sd_bus_message_append(message, "ss", unit.c_str(), "fail") >= 0
        && sd_bus_message_open_container(message, 'a', "(sv)") >= 0
        && sd_bus_message_append(
               message, "(sv)(sv)(sv)", "PIDs", "au", 1, static_cast<std::uint32_t>(pid), "Slice", "s", "app.slice",
               "CollectMode", "s", "inactive-or-failed"
           ) >= 0;
    if (built && !options.description.empty()) {
      built = sd_bus_message_append(message, "(sv)", "Description", "s", options.description.c_str()) >= 0;
    }
    for (const auto& [name, units] : {std::pair{"PartOf", &options.partOf}, std::pair{"After", &options.after}}) {
      if (!built || units->empty()) {
        continue;
      }
      built = sd_bus_message_open_container(message, 'r', "sv") >= 0 && sd_bus_message_append(message, "s", name) >= 0
          && sd_bus_message_open_container(message, 'v', "as") >= 0
          && sd_bus_message_open_container(message, 'a', "s") >= 0;
      for (const std::string& dependency : *units) {
        built = built && sd_bus_message_append(message, "s", dependency.c_str()) >= 0;
      }
      built = built && sd_bus_message_close_container(message) >= 0 && sd_bus_message_close_container(message) >= 0
          && sd_bus_message_close_container(message) >= 0;
    }
    if (built && options.cpuWeight) {
      built = sd_bus_message_append(message, "(sv)", "CPUWeight", "t", *options.cpuWeight) >= 0;
    }
    built = built && sd_bus_message_close_container(message) >= 0 && sd_bus_message_append(message, "a(sa(sv))", 0) >= 0;
    if (built) {
      built = call(message, [unit, done = std::move(done)](sd_bus_message* reply) {
        const bool ok = sd_bus_message_is_method_error(reply, nullptr) == 0;
        if (!ok) {
          logFailure("StartTransientUnit " + unit, reply);
        }
        done(ok);
      });
    }
    sd_bus_message_unref(message);
    return built;
  }

  void SystemdUnits::listUnits(const std::vector<std::string>& patterns, std::function<void(const std::string&)> found) {
    if (m_bus.get() == nullptr || patterns.empty()) {
      return;
    }
    sd_bus_message* message = nullptr;
    bool built = sd_bus_message_new_method_call(m_bus.get(), &message, kService, kPath, kManager, "ListUnitsByPatterns") >= 0
        && sd_bus_message_append(message, "as", 0) >= 0
        && sd_bus_message_open_container(message, 'a', "s") >= 0;
    for (const std::string& pattern : patterns) {
      built = built && sd_bus_message_append(message, "s", pattern.c_str()) >= 0;
    }
    if (built && sd_bus_message_close_container(message) >= 0) {
      call(message, [found = std::move(found)](sd_bus_message* reply) {
        if (sd_bus_message_is_method_error(reply, nullptr) != 0
            || sd_bus_message_enter_container(reply, 'a', "(ssssssouso)") < 0) {
          return;
        }
        const char* name = nullptr;
        while (sd_bus_message_read(
                   reply, "(ssssssouso)", &name, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                   nullptr, nullptr
               )
               > 0) {
          if (name != nullptr) {
            found(name);
          }
        }
      });
    }
    sd_bus_message_unref(message);
  }

} // namespace andromeda
