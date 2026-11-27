#pragma once

#include "config/managed_settings.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace andromeda {

  // The validated requests of dsk_settings_manager_v1 clients until they apply. A version 3 object's requests wait in
  // queue() for its commit; an older object's go straight to apply(). The manager applies take() once per event-loop
  // turn: one file write and one reload for everything committed in that turn.
  class SettingsCommits {
  public:
    using Owner = void*;
    struct SetRequest {
      std::string key;
      ManagedSettingValue value;
    };
    struct LayerRequest {
      std::string layer;
      std::string key;
      ManagedSettingValue value; // monostate removes the key from the layer
    };
    struct ClearLayerRequest {
      std::string layer;
    };
    using Request = std::variant<SetRequest, LayerRequest, ClearLayerRequest>;

    struct Set {
      Owner owner; // null once its owner is dropped
      std::string key;
      ManagedSettingValue value;
    };
    struct Turn {
      std::vector<Set> sets; // in request order, volatile keys included
      std::vector<std::pair<Owner, uint32_t>> applied; // commits in order
    };

    void queue(Owner owner, Request request);
    void apply(Owner owner, Request request);
    void commit(Owner owner, uint32_t generation);
    // Forgets the owner's uncommitted requests, layers and unsent commits. True when a layer went.
    bool drop(Owner owner);
    // What was applied since the last take.
    Turn take() { return std::exchange(m_turn, {}); }
    // Every layer's values, layers in order of first use.
    [[nodiscard]] std::vector<std::pair<std::string, ManagedSettingValue>> overlay() const;

  private:
    struct Layer {
      Owner owner = nullptr;
      std::string name;
      std::vector<std::pair<std::string, ManagedSettingValue>> values;
    };
    std::unordered_map<Owner, std::vector<Request>> m_uncommitted;
    std::vector<Layer> m_layers;
    Turn m_turn;
  };

} // namespace andromeda
