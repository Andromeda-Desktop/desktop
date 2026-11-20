#include "layer/layer_shell_permissions.h"

#include <cstdlib>
#include <nlohmann/json.hpp>

#include "core/log.h"
#include "util/private_state_file.h"

namespace andromeda {

  namespace {
    constexpr Logger kLog("layer");
    constexpr size_t kMaxApps = 256;
    constexpr size_t kMaxAppBytes = 512;
    constexpr size_t kMaxStoreBytes = 256 * 1024;
  } // namespace

  LayerShellPermissions::LayerShellPermissions(std::filesystem::path path) : m_path(std::move(path)) {
    if (m_path.empty() || !m_path.is_absolute()) {
      return;
    }
    const auto file = private_state::read(m_path, kMaxStoreBytes);
    if (!std::holds_alternative<std::string>(file)) {
      if (std::holds_alternative<private_state::Unsafe>(file)) {
        kLog.warn("ignoring unsafe layer-shell permissions file {}", m_path.string());
      }
      return;
    }
    const auto data = nlohmann::json::parse(std::get<std::string>(file), nullptr, false);
    if (!data.is_object() || data.value("version", 0) != 1 || !data.contains("apps") || !data["apps"].is_array()
        || data["apps"].size() > kMaxApps) {
      kLog.warn("ignoring malformed layer-shell permissions file {}", m_path.string());
      return;
    }
    for (const auto& app : data["apps"]) {
      if (app.is_string() && app.get_ref<const std::string&>().size() <= kMaxAppBytes) {
        m_apps.insert(app.get<std::string>());
      }
    }
  }

  std::filesystem::path LayerShellPermissions::defaultPath() {
    if (const char* state = std::getenv("XDG_STATE_HOME"); state != nullptr && *state != '\0') {
      return std::filesystem::path(state) / "andromeda/layer-shell-permissions.json";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
      return std::filesystem::path(home) / ".local/state/andromeda/layer-shell-permissions.json";
    }
    return {};
  }

  bool LayerShellPermissions::remember(const std::string& app) {
    if (app.size() > kMaxAppBytes || (m_apps.size() >= kMaxApps && !m_apps.contains(app))) {
      return false;
    }
    std::set<std::string> next = m_apps;
    next.insert(app);
    if (!save(next)) {
      return false;
    }
    m_apps = std::move(next);
    return true;
  }

  bool LayerShellPermissions::revoke(std::string_view app) {
    std::set<std::string> next = m_apps;
    if (app == "all") {
      next.clear();
    } else if (next.erase(std::string(app)) == 0) {
      return false;
    }
    if (!save(next)) {
      return false;
    }
    m_apps = std::move(next);
    return true;
  }

  bool LayerShellPermissions::save(const std::set<std::string>& apps) const {
    if (m_path.empty() || !m_path.is_absolute()) {
      return false;
    }
    std::string bytes;
    try {
      bytes = nlohmann::json{{"version", 1}, {"apps", apps}}.dump() + '\n';
    } catch (const nlohmann::json::exception&) {
      return false;
    }
    return private_state::writeAtomic(m_path, bytes);
  }

} // namespace andromeda
