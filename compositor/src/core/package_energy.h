#pragma once

#include "core/loop_bus.h"

#include <functional>

struct sd_bus_message;
struct wl_event_loop;

namespace andromeda {

  // Reads org.andromeda.System1.Power.PackageEnergyJ or UncoreEnergyJ (RAPL joules since that daemon started; RAPL
  // is root-only) over the system bus without blocking the event loop.
  class PackageEnergy {
  public:
    explicit PackageEnergy(wl_event_loop* loop);

    // `done` gets the joules of `method`, or NaN when the daemon or RAPL is missing. A request while one is pending
    // is dropped; `done` may start the next one.
    void request(const char* method, std::function<void(double)> done);

  private:
    void finish(sd_bus_message* reply);

    bus::Client m_bus;
    std::function<void(double)> m_done;
  };

} // namespace andromeda
