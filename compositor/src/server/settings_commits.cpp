#include "server/settings_commits.h"

#include <algorithm>

namespace andromeda {

  void SettingsCommits::queue(Owner owner, Request request) { m_uncommitted[owner].push_back(std::move(request)); }

  void SettingsCommits::apply(Owner owner, Request request) {
    if (auto* set = std::get_if<SetRequest>(&request)) {
      m_turn.sets.push_back({.owner = owner, .key = std::move(set->key), .value = std::move(set->value)});
      return;
    }
    if (auto* clear = std::get_if<ClearLayerRequest>(&request)) {
      std::erase_if(m_layers, [&](const Layer& l) { return l.owner == owner && l.name == clear->layer; });
      return;
    }
    auto& entry = std::get<LayerRequest>(request);
    const bool unset = std::holds_alternative<std::monostate>(entry.value);
    auto it = std::ranges::find_if(m_layers, [&](const Layer& l) { return l.owner == owner && l.name == entry.layer; });
    if (it == m_layers.end()) {
      if (unset) {
        return;
      }
      it = m_layers.insert(m_layers.end(), Layer{.owner = owner, .name = std::move(entry.layer), .values = {}});
    }
    std::erase_if(it->values, [&](const auto& value) { return value.first == entry.key; });
    if (!unset) {
      it->values.emplace_back(std::move(entry.key), std::move(entry.value));
    }
    if (it->values.empty()) {
      m_layers.erase(it);
    }
  }

  void SettingsCommits::commit(Owner owner, uint32_t generation) {
    if (const auto it = m_uncommitted.find(owner); it != m_uncommitted.end()) {
      for (Request& request : it->second) {
        apply(owner, std::move(request));
      }
      m_uncommitted.erase(it);
    }
    m_turn.applied.emplace_back(owner, generation);
  }

  bool SettingsCommits::drop(Owner owner) {
    m_uncommitted.erase(owner);
    std::erase_if(m_turn.applied, [owner](const auto& entry) { return entry.first == owner; });
    for (Set& set : m_turn.sets) {
      if (set.owner == owner) {
        set.owner = nullptr;
      }
    }
    return std::erase_if(m_layers, [owner](const Layer& l) { return l.owner == owner; }) != 0;
  }

  std::vector<std::pair<std::string, ManagedSettingValue>> SettingsCommits::overlay() const {
    std::vector<std::pair<std::string, ManagedSettingValue>> flat;
    for (const Layer& layer : m_layers) {
      flat.insert(flat.end(), layer.values.begin(), layer.values.end());
    }
    return flat;
  }

} // namespace andromeda
