#include "core/package_energy.h"
#include "protocols/bus_names.h"

#include <cmath>
#include <cstdint>
#include <utility>

#include <systemd/sd-bus.h>

namespace andromeda {

  namespace {

    constexpr std::uint64_t kCallTimeoutUs = 5'000'000;

  } // namespace

  PackageEnergy::PackageEnergy(wl_event_loop* loop) : m_bus(loop, bus::Kind::System) {}

  void PackageEnergy::request(const char* method, std::function<void(double)> done) {
    if (m_done) {
      return;
    }
    sd_bus_message* message = nullptr;
    if (m_bus.get() == nullptr
        || sd_bus_message_new_method_call(
               m_bus.get(), &message, busname::kSystem1, "/org/andromeda/System1/Power",
               "org.andromeda.System1.Power", method
           ) < 0) {
      done(NAN);
      return;
    }
    m_done = std::move(done);
    const bool sent = m_bus.call(message, [this](sd_bus_message* reply) { finish(reply); }, kCallTimeoutUs);
    sd_bus_message_unref(message);
    if (!sent) {
      finish(nullptr);
    }
  }

  void PackageEnergy::finish(sd_bus_message* reply) {
    double joules = NAN;
    if (reply == nullptr || sd_bus_message_is_method_error(reply, nullptr) != 0
        || sd_bus_message_read(reply, "d", &joules) < 0) {
      joules = NAN;
    }
    if (auto done = std::exchange(m_done, nullptr); done) {
      done(joules);
    }
  }

} // namespace andromeda
