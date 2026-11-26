#include "layer/layer_shell_permissions.h"

#include "core/log.h"
#include "protocols/xdg_dirs.h"
#include "util/private_state_file.h"

#include <cstdlib>
#include <nlohmann/json.hpp>
#include <optional>

namespace andromeda {

  namespace {
    constexpr Logger kLog("layer");
    constexpr size_t kMaxApps = 256;
    constexpr size_t kMaxAppBytes = 512;
    constexpr size_t kMaxStoreBytes = 256 * 1024;
    constexpr std::string_view kLockName = ".layer-shell-permissions.lock";

    // Missing = no apps; nullopt = unsafe or malformed.
    std::optional<std::set<std::string>> parse(const private_state::ReadResult& file) {
      if (std::holds_alternative<private_state::Missing>(file)) {
        return std::set<std::string>{};
      }
      if (!std::holds_alternative<std::string>(file)) {
        return std::nullopt;
      }
      const auto data = nlohmann::json::parse(std::get<std::string>(file), nullptr, false);
      if (!data.is_object()
          || data.value("version", 0) != 1
          || !data.contains("apps")
          || !data["apps"].is_array()
          || data["apps"].size() > kMaxApps) {
        return std::nullopt;
      }
      std::set<std::string> apps;
      for (const auto& app : data["apps"]) {
        if (app.is_string() && app.get_ref<const std::string&>().size() <= kMaxAppBytes) {
          apps.insert(app.get<std::string>());
        }
      }
      return apps;
    }

    std::optional<std::string> serialize(const std::set<std::string>& apps) {
      try {
        return nlohmann::json{{"version", 1}, {"apps", apps}}.dump() + '\n';
      } catch (const nlohmann::json::exception&) {
        return std::nullopt;
      }
    }

    // The apps after one change, and whether it changed anything.
    struct Changed {
      std::set<std::string> apps;
      bool changed = false;
    };

    // Merges into the file as read now (another compositor instance may have written it); an unsafe or malformed
    // file is replaced, starting from what this instance holds.
    std::function<private_state::Merge<Changed>(const private_state::ReadResult&)>
    merger(std::set<std::string> held, std::function<bool(std::set<std::string>&)> change) {
      return [held = std::move(held), change = std::move(change)](const private_state::ReadResult& file) {
        auto apps = parse(file).value_or(held);
        if (!change(apps)) {
          return private_state::Merge<Changed>{std::nullopt, {std::move(apps), false}};
        }
        auto bytes = serialize(apps);
        const bool saved = bytes.has_value();
        return private_state::Merge<Changed>{std::move(bytes), {std::move(apps), saved}};
      };
    }
  } // namespace

  LayerShellPermissions::LayerShellPermissions(wl_event_loop* loop, std::filesystem::path path)
      : m_saver(loop, std::move(path), std::string(kLockName), kMaxStoreBytes, "andro-layerperm") {
    const auto& file = m_saver.file();
    if (file.empty() || !file.is_absolute()) {
      return;
    }
    const auto read = private_state::read(file, kMaxStoreBytes);
    auto apps = parse(read);
    if (!apps) {
      kLog.warn(
          "ignoring {} layer-shell permissions file {}",
          std::holds_alternative<private_state::Unsafe>(read) ? "unsafe" : "malformed", file.string()
      );
      return;
    }
    m_apps = std::move(*apps);
  }

  std::filesystem::path LayerShellPermissions::defaultPath() {
    if (const std::string state = xdg::stateHome(); !state.empty()) {
      return std::filesystem::path(state) / "andromeda/layer-shell-permissions.json";
    }
    return {};
  }

  void LayerShellPermissions::remember(const std::string& app, Done done) {
    if (app.size() > kMaxAppBytes) {
      m_saver.then([done = std::move(done)] { done(false); });
      return;
    }
    m_saver.save<Changed>(
        merger(
            m_apps,
            [app](std::set<std::string>& apps) {
              return (apps.size() < kMaxApps || apps.contains(app)) && apps.insert(app).second;
            }
        ),
        [this, app, done = std::move(done)](std::optional<Changed> result) {
          if (result) {
            m_apps = std::move(result->apps);
          }
          // Already remembered on disk is saved too.
          done(result && m_apps.contains(app));
        }
    );
  }

  void LayerShellPermissions::revoke(std::string_view app, std::function<void(std::optional<bool> removed)> done) {
    ++m_revoking[std::string(app)];
    m_saver.save<Changed>(
        merger(
            m_apps,
            [app = std::string(app)](std::set<std::string>& apps) {
              if (app == "all") {
                apps.clear();
                return true;
              }
              return apps.erase(app) != 0;
            }
        ),
        [this, app = std::string(app), done = std::move(done)](std::optional<Changed> result) {
          if (--m_revoking[app] == 0) {
            m_revoking.erase(app);
          }
          if (!result) {
            done(std::nullopt);
            return;
          }
          m_apps = std::move(result->apps);
          done(result->changed);
        }
    );
  }

} // namespace andromeda
