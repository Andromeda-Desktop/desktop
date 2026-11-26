#include "input/global_shortcut_store.h"

#include "protocols/xdg_dirs.h"
#include "util/private_state_file.h"

#include <cstdlib>
#include <nlohmann/json.hpp>
#include <set>
#include <variant>

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

    std::optional<ShortcutGrants> parseFile(const private_state::ReadResult& file) {
      if (std::holds_alternative<private_state::Missing>(file)) {
        return ShortcutGrants{};
      }
      const auto* bytes = std::get_if<std::string>(&file);
      return bytes != nullptr ? parse(*bytes) : std::nullopt;
    }

    std::optional<std::string> serialize(const ShortcutGrants& grants) {
      if (grants.size() > kMaxApps) {
        return std::nullopt;
      }
      auto data = nlohmann::json{{"version", 1}, {"grants", nlohmann::json::object()}};
      for (const auto& [name, entries] : grants) {
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
      try {
        return data.dump() + '\n';
      } catch (const nlohmann::json::exception&) {
        return std::nullopt;
      }
    }

    // The file was unsafe or malformed; the store stops writing it.
    struct Unreadable {};
    // Over a cap: the file is left as it is.
    struct Refused {};
    // The grants as written, or why nothing was written.
    using Updated = std::variant<ShortcutGrants, Unreadable, Refused>;
  } // namespace

  GlobalShortcutStore::GlobalShortcutStore(wl_event_loop* loop, std::filesystem::path path)
      : m_saver(loop, std::move(path), std::string(kLockName), kMaxStoreBytes, "andro-shortcuts") {}

  std::filesystem::path GlobalShortcutStore::defaultPath() {
    if (const std::string state = xdg::stateHome(); !state.empty()) {
      return std::filesystem::path(state) / "andromeda/global-shortcuts.json";
    }
    return {};
  }

  std::optional<ShortcutGrants> GlobalShortcutStore::load() {
    auto grants = parseFile(private_state::read(path(), kMaxStoreBytes));
    m_status = grants ? Status::Loaded : Status::Unreadable;
    return grants;
  }

  void GlobalShortcutStore::update(std::string_view app, std::map<std::string, ShortcutChord> shortcuts, Done done) {
    if (m_status != Status::Loaded || (app.empty() && !shortcuts.empty())) {
      m_saver.then([done = std::move(done)] { done(std::nullopt); });
      return;
    }
    m_saver.save<Updated>(
        [app = std::string(app), shortcuts = std::move(shortcuts)](const private_state::ReadResult& file) {
          auto grants = parseFile(file);
          if (!grants) {
            return private_state::Merge<Updated>{std::nullopt, Unreadable{}};
          }
          if (app.empty()) {
            grants->clear();
          } else if (shortcuts.empty()) {
            grants->erase(app);
          } else {
            (*grants)[app] = shortcuts;
          }
          auto bytes = serialize(*grants);
          if (!bytes) {
            return private_state::Merge<Updated>{std::nullopt, Refused{}};
          }
          return private_state::Merge<Updated>{std::move(bytes), std::move(*grants)};
        },
        [this, done = std::move(done)](std::optional<Updated> result) {
          if (result && std::holds_alternative<Unreadable>(*result)) {
            m_status = Status::Unreadable;
          }
          auto* grants = result ? std::get_if<ShortcutGrants>(&*result) : nullptr;
          done(grants != nullptr ? std::optional<ShortcutGrants>(std::move(*grants)) : std::nullopt);
        }
    );
  }

  ShortcutGrantSync::ShortcutGrantSync(ShortcutRegistry& registry, wl_event_loop* loop, std::filesystem::path path)
      : m_registry(registry), m_store(loop, std::move(path)) {}

  bool ShortcutGrantSync::load() {
    auto grants = m_store.load();
    if (grants) {
      m_file = *grants;
      m_registry.setGrants(std::move(*grants));
    }
    return grants.has_value();
  }

  void ShortcutGrantSync::persist(const std::string& app, Done done) {
    const auto& grants = m_registry.grants();
    const auto found = grants.find(app);
    save(
        app, found == grants.end() ? std::map<std::string, ShortcutChord>{} : found->second,
        [this, app, done = std::move(done)](bool saved) {
          if (saved) {
            m_unsaved.erase(app);
          } else {
            m_unsaved.insert(app);
          }
          done(saved);
        }
    );
  }

  void ShortcutGrantSync::revoke(std::string_view app, Done done) {
    m_registry.revoke(app);
    save(std::string(app), {}, [this, app = std::string(app), done = std::move(done)](bool saved) {
      // Saved or not, memory now follows the file for these apps.
      if (app.empty()) {
        m_unsaved.clear();
      } else {
        m_unsaved.erase(app);
      }
      // A later revoke of all settles these apps itself.
      if (!saved && !m_saving.contains("")) {
        ShortcutGrants grants = m_registry.grants();
        std::set<std::string> apps;
        for (const auto* known : {&grants, &m_file}) {
          for (const auto& [granted, shortcuts] : *known) {
            if (app.empty() || granted == app) {
              apps.insert(granted);
            }
          }
        }
        if (!app.empty()) {
          apps.insert(app);
        }
        for (const std::string& granted : apps) {
          if (m_saving.contains(granted)) {
            continue;
          }
          if (const auto stored = m_file.find(granted); stored != m_file.end()) {
            grants[granted] = stored->second;
          } else {
            grants.erase(granted);
          }
        }
        m_registry.setGrants(std::move(grants));
      }
      done(saved);
    });
  }

  void ShortcutGrantSync::save(const std::string& app, std::map<std::string, ShortcutChord> grants, Done done) {
    ++m_saving[app];
    m_store.update(app, std::move(grants), [this, app, done = std::move(done)](std::optional<ShortcutGrants> merged) {
      if (--m_saving[app] == 0) {
        m_saving.erase(app);
      }
      if (!merged) {
        done(false);
        return;
      }
      m_file = *merged;
      if (m_saving.contains("")) {
        // A queued revoke of all: memory already dropped every grant the file still holds.
        *merged = m_registry.grants();
      }
      std::set<std::string> keep(m_unsaved);
      for (const auto& [queued, count] : m_saving) {
        keep.insert(queued);
      }
      for (const std::string& kept : keep) {
        const auto live = m_registry.grants().find(kept);
        if (live != m_registry.grants().end()) {
          (*merged)[kept] = live->second;
        } else {
          merged->erase(kept);
        }
      }
      m_registry.setGrants(std::move(*merged));
      done(true);
    });
  }

} // namespace andromeda
