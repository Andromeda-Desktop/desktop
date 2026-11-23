#pragma once

#include "core/loop_bus.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct sd_bus;
struct sd_bus_message;
struct wl_event_loop;
struct wl_event_source;

namespace andromeda {

  // The compositor's one client of the systemd user manager. Calls are asynchronous on the compositor's event loop,
  // so the compositor never waits on systemd. Without sd-bus or a user bus every call is a no-op.
  class SystemdUnits {
  public:
    // Runtime unit properties; an unset field is left as it is.
    struct Properties {
      std::optional<std::uint64_t> cpuWeight;
      std::optional<std::uint64_t> ioWeight;
      std::optional<std::vector<int>> allowedCpus; // empty = every CPU
      std::optional<std::uint64_t> memoryLow;      // bytes
      std::optional<std::uint64_t> memoryMin;      // bytes
    };
    // Extra properties of a new scope; an empty field is not sent.
    struct ScopeOptions {
      std::string description;
      std::vector<std::string> partOf;
      std::vector<std::string> after;
      std::optional<std::uint64_t> cpuWeight;
    };

    explicit SystemdUnits(wl_event_loop* loop);
    virtual ~SystemdUnits();
    SystemdUnits(const SystemdUnits&) = delete;
    SystemdUnits& operator=(const SystemdUnits&) = delete;

    [[nodiscard]] bool active() const { return m_bus.get() != nullptr; }
    virtual void setProperties(const std::string& unit, const Properties& properties);
    // `done` gets true once systemd reports the unit frozen or thawed; it never runs after the bus is gone.
    // Returns false when the call was not sent; `done` then never runs.
    virtual bool setFrozen(const std::string& unit, bool frozen, std::function<void(bool ok)> done = {});
    // Moves `pid` into a new transient scope `unit` under app.slice. `done` gets true once systemd started it.
    // Returns false when the call was not sent; `done` then never runs.
    virtual bool startScope(
        const std::string& unit, int pid, std::function<void(bool ok)> done, const ScopeOptions& options = {}
    );
    // Calls `found` once for each loaded unit whose name matches one of the patterns.
    void listUnits(const std::vector<std::string>& patterns, std::function<void(const std::string&)> found);

  private:
    using Reply = std::function<void(sd_bus_message*)>;
    bool call(sd_bus_message* message, Reply reply);

    bus::Client m_bus;
  };

} // namespace andromeda
