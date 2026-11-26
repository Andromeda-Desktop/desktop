#pragma once

#include "util/private_state_file.h"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

struct wl_event_loop;

namespace andromeda {

  // Sandboxed apps ("sandbox_engine/app_id") the user allowed to use layer-shell until revoked.
  class LayerShellPermissions {
  public:
    using Done = std::function<void(bool saved)>;

    LayerShellPermissions(wl_event_loop* loop, std::filesystem::path path);
    static std::filesystem::path defaultPath();

    // False from the moment a revoke of the app is requested until its save answers.
    [[nodiscard]] bool contains(const std::string& app) const {
      return m_apps.contains(app) && !m_revoking.contains(app) && !m_revoking.contains("all");
    }
    [[nodiscard]] const std::set<std::string>& apps() const { return m_apps; }
    // Saved in request order; afterwards memory holds the file's apps. `done` gets false when the state file cannot
    // be written.
    void remember(const std::string& app, Done done);
    // `all` forgets every app. `done` gets whether the file held the app (always true for `all`), or nullopt when the
    // state file could not be read or written.
    void revoke(std::string_view app, std::function<void(std::optional<bool> removed)> done);

  private:
    std::set<std::string> m_apps;
    // Revokes queued per app ("all": every app).
    std::map<std::string, int> m_revoking;
    private_state::Saver m_saver;
  };

} // namespace andromeda
