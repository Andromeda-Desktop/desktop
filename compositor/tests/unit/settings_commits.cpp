// Model test of SettingsCommits: random interleavings of version 2 requests, version 3 requests and commits, client
// drops and event-loop turns. Every request carries a unique value id, so each visible value names its request.
#include "check.h"
#include "server/settings_commits.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <tuple>

using andromeda::ManagedSettingValue;
using andromeda::SettingsCommits;

namespace {

  constexpr std::array<const char*, 2> kKeys{"a", "b"};
  constexpr std::array<const char*, 2> kLayers{"L", "M"};

  struct Model {
    std::array<int, 3> storage{};
    // Owner 0 speaks version 2 (each request applies alone); owners 1 and 2 speak version 3.
    [[nodiscard]] SettingsCommits::Owner owner(size_t i) { return &storage[i]; }
    static bool committing(size_t i) { return i != 0; }

    std::array<std::vector<SettingsCommits::Request>, 3> pending;
    // Last applied value per (owner, layer, key): an id, or nullopt once removed.
    std::map<std::tuple<size_t, std::string, std::string>, std::optional<int64_t>> slots;
    // Creation sequence of each non-empty (owner, layer): the overlay keeps layers in this order.
    std::map<std::pair<size_t, std::string>, int> layerSeq;
    int nextSeq = 0;
    std::vector<std::pair<int64_t, std::optional<size_t>>> sets; // id, owner (nullopt once dropped)
    std::vector<std::pair<size_t, uint32_t>> commits;
    std::map<int64_t, std::pair<size_t, std::string>> idLayer;

    bool layerEmpty(size_t o, const std::string& layer) const {
      return std::ranges::none_of(slots, [&](const auto& entry) {
        return std::get<0>(entry.first) == o && std::get<1>(entry.first) == layer && entry.second.has_value();
      });
    }

    void applied(size_t o, const SettingsCommits::Request& request) {
      if (const auto* set = std::get_if<SettingsCommits::SetRequest>(&request)) {
        sets.emplace_back(std::get<int64_t>(set->value), o);
        return;
      }
      if (const auto* clear = std::get_if<SettingsCommits::ClearLayerRequest>(&request)) {
        for (auto& [slot, value] : slots) {
          if (std::get<0>(slot) == o && std::get<1>(slot) == clear->layer) {
            value.reset();
          }
        }
        layerSeq.erase({o, clear->layer});
        return;
      }
      const auto& entry = std::get<SettingsCommits::LayerRequest>(request);
      const bool wasEmpty = layerEmpty(o, entry.layer);
      std::optional<int64_t> value;
      if (const auto* id = std::get_if<int64_t>(&entry.value)) {
        value = *id;
        idLayer[*id] = {o, entry.layer};
      }
      slots[{o, entry.layer, entry.key}] = value;
      if (wasEmpty && value.has_value()) {
        layerSeq[{o, entry.layer}] = nextSeq++;
      } else if (layerEmpty(o, entry.layer)) {
        layerSeq.erase({o, entry.layer});
      }
    }
  };

  // The overlay shows exactly the last applied value of each slot, never a queued one, layers in creation order.
  void checkOverlay(Model& model, const SettingsCommits& commits) {
    std::vector<int64_t> expected;
    for (const auto& [slot, value] : model.slots) {
      if (value.has_value()) {
        expected.push_back(*value);
      }
    }
    std::vector<int64_t> visible;
    int lastSeq = -1;
    for (const auto& [key, value] : commits.overlay()) {
      const int64_t id = std::get<int64_t>(value);
      visible.push_back(id);
      const auto layer = model.idLayer.find(id);
      const auto seq = layer != model.idLayer.end() ? model.layerSeq.find(layer->second) : model.layerSeq.end();
      CHECK(seq != model.layerSeq.end());
      if (seq != model.layerSeq.end()) {
        CHECK(seq->second >= lastSeq);
        lastSeq = seq->second;
      }
    }
    std::ranges::sort(expected);
    std::ranges::sort(visible);
    CHECK(visible == expected);
  }

  void checkTurn(Model& model, SettingsCommits& commits) {
    const SettingsCommits::Turn turn = commits.take();
    CHECK_EQ(turn.sets.size(), model.sets.size());
    for (size_t i = 0; i < std::min(turn.sets.size(), model.sets.size()); ++i) {
      CHECK_EQ(std::get<int64_t>(turn.sets[i].value), model.sets[i].first);
      const auto& owner = model.sets[i].second;
      CHECK(turn.sets[i].owner == (owner.has_value() ? model.owner(*owner) : nullptr));
    }
    CHECK_EQ(turn.applied.size(), model.commits.size());
    for (size_t i = 0; i < std::min(turn.applied.size(), model.commits.size()); ++i) {
      CHECK(turn.applied[i].first == model.owner(model.commits[i].first));
      CHECK_EQ(turn.applied[i].second, model.commits[i].second);
    }
    model.sets.clear();
    model.commits.clear();
  }

} // namespace

ANDROMEDA_TEST(randomInterleavingsKeepEveryCommitInvariant) {
  for (uint32_t seed = 1; seed <= 40; ++seed) {
    std::mt19937 random(seed);
    const auto pick = [&](size_t n) { return static_cast<size_t>(random() % n); };
    Model model;
    SettingsCommits commits;
    int64_t nextId = 1;
    uint32_t generation = 0;
    for (int step = 0; step < 400; ++step) {
      const size_t o = pick(3);
      const size_t action = pick(10);
      if (action < 6) {
        SettingsCommits::Request request;
        const std::string key = kKeys[pick(kKeys.size())];
        const std::string layer = kLayers[pick(kLayers.size())];
        const size_t kind = pick(6);
        if (kind == 0) {
          request = SettingsCommits::SetRequest{.key = key, .value = nextId++};
        } else if (kind == 1) {
          request = SettingsCommits::ClearLayerRequest{.layer = layer};
        } else {
          const ManagedSettingValue value = kind == 2 ? ManagedSettingValue{} : ManagedSettingValue{nextId++};
          request = SettingsCommits::LayerRequest{.layer = layer, .key = key, .value = value};
        }
        if (Model::committing(o)) {
          model.pending[o].push_back(request);
          commits.queue(model.owner(o), std::move(request));
        } else {
          model.applied(o, request);
          commits.apply(model.owner(o), std::move(request));
        }
      } else if (action < 8 && Model::committing(o)) {
        for (const auto& request : model.pending[o]) {
          model.applied(o, request);
        }
        model.pending[o].clear();
        model.commits.emplace_back(o, ++generation);
        commits.commit(model.owner(o), generation);
      } else if (action == 8) {
        model.pending[o].clear();
        for (auto& [slot, value] : model.slots) {
          if (std::get<0>(slot) == o) {
            value.reset();
          }
        }
        std::erase_if(model.layerSeq, [o](const auto& entry) { return entry.first.first == o; });
        for (auto& [id, owner] : model.sets) {
          if (owner == o) {
            owner.reset();
          }
        }
        std::erase_if(model.commits, [o](const auto& entry) { return entry.first == o; });
        commits.drop(model.owner(o));
      } else {
        checkTurn(model, commits);
      }
      checkOverlay(model, commits);
    }
    checkTurn(model, commits);
  }
}

int main() { return RUN_TESTS(); }
