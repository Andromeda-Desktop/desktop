#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <string_view>

namespace andromeda {

  // Sandboxed apps ("sandbox_engine/app_id") the user allowed to use layer-shell until revoked.
  class LayerShellPermissions {
  public:
    explicit LayerShellPermissions(std::filesystem::path path);
    static std::filesystem::path defaultPath();

    [[nodiscard]] bool contains(const std::string& app) const { return m_apps.contains(app); }
    [[nodiscard]] const std::set<std::string>& apps() const { return m_apps; }
    // Both return false when the state file cannot be written; memory then keeps the old set.
    bool remember(const std::string& app);
    // `all` forgets every app.
    bool revoke(std::string_view app);

  private:
    bool save(const std::set<std::string>& apps) const;

    std::filesystem::path m_path;
    std::set<std::string> m_apps;
  };

} // namespace andromeda
