#include "input/global_shortcut_store.h"

#include "protocols/xdg_dirs.h"
#include "util/private_state_file.h"

#include <cstdlib>
#include <nlohmann/json.hpp>

namespace andromeda {

  namespace {
    constexpr size_t kMaxApps = 128;
    constexpr size_t kMaxIdsPerApp = 64;
    constexpr size_t kMaxNameBytes = 256;
    constexpr size_t kMaxStoreBytes = 256 * 1024;
    constexpr std::string_view kLockName = ".global-shortcuts.lock";

    bool validName(std::string_view name) { return !name.empty() && name.size() <= kMaxNameBytes; }

    std::optional<ShortcutGrants> parse(const std::string& bytes) {
      const auto data = nlohmann::json::parse(bytes, nullptr, false);
      if (!data.is_object() || data.size() != 2 || !data.contains("version") || !data["version"].is_number_unsigned()
          || data["version"] != 1 || !data.contains("grants") || !data["grants"].is_object()
          || data["grants"].size() > kMaxApps) {
        return std::nullopt;
      }
      ShortcutGrants grants;
      for (const auto& [app, shortcuts] : data["grants"].items()) {
        if (!validName(app) || !shortcuts.is_object() || shortcuts.empty() || shortcuts.size() > kMaxIdsPerApp) {
          return std::nullopt;
        }
        auto& target = grants[app];
        for (const auto& [id, text] : shortcuts.items()) {
          if (!validName(id) || !text.is_string()) {
            return std::nullopt;
          }
          // describeChord writes the portal trigger syntax, so the portal parser reads it back.
          const auto chord = parsePortalTrigger(text.get<std::string>());
          if (!chord) {
            return std::nullopt;
          }
          target[id] = *chord;
        }
      }
      return grants;
    }
  } // namespace

  GlobalShortcutStore::GlobalShortcutStore(std::filesystem::path path) : m_path(std::move(path)) {}

  std::filesystem::path GlobalShortcutStore::defaultPath() {
    if (const std::string state = xdg::stateHome(); !state.empty()) {
      return std::filesystem::path(state) / "andromeda/global-shortcuts.json";
    }
    return {};
  }

  std::optional<ShortcutGrants> GlobalShortcutStore::load() {
    const auto file = private_state::read(m_path, kMaxStoreBytes);
    std::optional<ShortcutGrants> grants;
    if (std::holds_alternative<private_state::Missing>(file)) {
      grants.emplace();
    } else if (const auto* bytes = std::get_if<std::string>(&file)) {
      grants = parse(*bytes);
    }
    m_status = grants ? Status::Loaded : Status::Unreadable;
    return grants;
  }

  std::optional<ShortcutGrants> GlobalShortcutStore::update(
      std::string_view app, const std::map<std::string, ShortcutChord>& shortcuts
  ) {
    if (m_status != Status::Loaded || (app.empty() && !shortcuts.empty())) {
      return std::nullopt;
    }
    const private_state::Lock lock(m_path, kLockName);
    if (!lock) {
      return std::nullopt;
    }
    auto grants = load();
    if (!grants) {
      return std::nullopt;
    }
    if (app.empty()) {
      grants->clear();
    } else if (shortcuts.empty()) {
      grants->erase(std::string(app));
    } else {
      (*grants)[std::string(app)] = shortcuts;
    }
    if (grants->size() > kMaxApps) {
      return std::nullopt;
    }
    auto data = nlohmann::json{{"version", 1}, {"grants", nlohmann::json::object()}};
    for (const auto& [name, entries] : *grants) {
      if (!validName(name) || entries.size() > kMaxIdsPerApp) {
        return std::nullopt;
      }
      auto& entry = data["grants"][name];
      for (const auto& [id, chord] : entries) {
        const std::string text = describeChord(chord);
        if (!validName(id) || parsePortalTrigger(text) != chord) {
          return std::nullopt;
        }
        entry[id] = text;
      }
    }
    std::string bytes;
    try {
      bytes = data.dump() + '\n';
    } catch (const nlohmann::json::exception&) {
      return std::nullopt;
    }
    if (bytes.size() > kMaxStoreBytes || !private_state::writeAtomic(m_path, bytes)) {
      return std::nullopt;
    }
    return grants;
  }

} // namespace andromeda
