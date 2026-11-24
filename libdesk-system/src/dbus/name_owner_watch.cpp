#include "dbus/name_owner_watch.h"

#include "core/log.h"
#include "protocols/bus_names.h"

#include <sdbus-c++/sdbus-c++.h>
#include <optional>
#include <utility>

namespace {
  constexpr Logger kLog("bus-owner");

  std::string ownerRule(const char* argKey, const std::string& value) {
    std::string rule = std::string("type='signal',sender='") + busname::kDaemon + "',path='" + busname::kDaemonPath +
                       "',interface='" + busname::kDaemon + "',member='NameOwnerChanged'";
    if (argKey != nullptr) {
      rule += std::string(",") + argKey + "='" + value + "'";
    }
    return rule;
  }
} // namespace

namespace {
  // Signals and the GetNameOwner reply arrive in bus order: the first always reports (from ""), later ones only
  // when the owner differs.
  class NameOwnerTracker {
  public:
    struct Change {
      std::string oldOwner;
      std::string newOwner;
    };
    [[nodiscard]] std::optional<Change> owner(const std::string& owner) {
      if (m_owner == owner) {
        return std::nullopt;
      }
      std::string oldOwner = std::exchange(m_owner, owner).value_or(std::string());
      return Change{.oldOwner = std::move(oldOwner), .newOwner = owner};
    }

  private:
    std::optional<std::string> m_owner;
  };
} // namespace

struct NameOwnerWatch::State {
  NameOwnerWatch::Callback callback;
  sdbus::Slot match;
  std::unique_ptr<sdbus::IProxy> daemon;
  // Only for a one-name watch.
  std::optional<NameOwnerTracker> tracker;
  std::string name;

  void report(const std::string& changed, const std::optional<NameOwnerTracker::Change>& change) {
    if (change.has_value()) {
      callback(changed, change->oldOwner, change->newOwner);
    }
  }
};

bool nameHasOwner(sdbus::IConnection& bus, const std::string& name) {
  auto daemon = sdbus::createProxy(bus, sdbus::ServiceName{busname::kDaemon}, sdbus::ObjectPath{busname::kDaemonPath});
  bool owned = false;
  daemon->callMethod("NameHasOwner").onInterface(busname::kDaemon).withArguments(name).storeResultsTo(owned);
  return owned;
}

NameOwnerWatch::NameOwnerWatch() = default;
NameOwnerWatch::NameOwnerWatch(NameOwnerWatch&&) noexcept = default;
NameOwnerWatch& NameOwnerWatch::operator=(NameOwnerWatch&&) noexcept = default;
NameOwnerWatch::~NameOwnerWatch() = default;

namespace {
  sdbus::Slot subscribe(sdbus::IConnection& bus, const std::string& rule, NameOwnerWatch::State* state) {
    return bus.addMatch(
        rule,
        [state](sdbus::Message message) {
          std::string name;
          std::string oldOwner;
          std::string newOwner;
          try {
            message >> name >> oldOwner >> newOwner;
          } catch (const sdbus::Error& e) {
            kLog.warn("NameOwnerChanged: {}", e.getMessage());
            return;
          }
          if (state->tracker.has_value()) {
            state->report(name, state->tracker->owner(newOwner));
          } else {
            state->callback(name, oldOwner, newOwner);
          }
        },
        sdbus::return_slot
    );
  }
} // namespace

NameOwnerWatch NameOwnerWatch::name(sdbus::IConnection& bus, std::string name, Callback callback) {
  NameOwnerWatch watch;
  watch.m_state = std::make_unique<State>();
  State* state = watch.m_state.get();
  state->callback = std::move(callback);
  state->tracker.emplace();
  state->name = std::move(name);
  state->match = subscribe(bus, ownerRule("arg0", state->name), state);
  // Bus order: the reply follows every signal the daemon sent before it, so it is the newest owner when it arrives.
  state->daemon = sdbus::createProxy(bus, sdbus::ServiceName{busname::kDaemon}, sdbus::ObjectPath{busname::kDaemonPath});
  state->daemon->callMethodAsync("GetNameOwner")
      .onInterface(busname::kDaemon)
      .withArguments(state->name)
      .uponReplyInvoke([state](std::optional<sdbus::Error> error, const std::string& owner) {
        if (error.has_value() && error->getName() != "org.freedesktop.DBus.Error.NameHasNoOwner") {
          kLog.warn("GetNameOwner {}: {}", state->name, error->getMessage());
          return;
        }
        state->report(state->name, state->tracker->owner(error.has_value() ? std::string() : owner));
      });
  return watch;
}

NameOwnerWatch NameOwnerWatch::changes(sdbus::IConnection& bus, std::string name, Callback callback) {
  NameOwnerWatch watch;
  watch.m_state = std::make_unique<State>();
  watch.m_state->callback = std::move(callback);
  watch.m_state->match = subscribe(bus, ownerRule("arg0", name), watch.m_state.get());
  return watch;
}

NameOwnerWatch NameOwnerWatch::prefix(sdbus::IConnection& bus, std::string prefix, Callback callback) {
  NameOwnerWatch watch;
  watch.m_state = std::make_unique<State>();
  watch.m_state->callback = std::move(callback);
  watch.m_state->match = subscribe(bus, ownerRule("arg0namespace", prefix), watch.m_state.get());
  return watch;
}

NameOwnerWatch NameOwnerWatch::all(sdbus::IConnection& bus, Callback callback) {
  NameOwnerWatch watch;
  watch.m_state = std::make_unique<State>();
  watch.m_state->callback = std::move(callback);
  watch.m_state->match = subscribe(bus, ownerRule(nullptr, {}), watch.m_state.get());
  return watch;
}
