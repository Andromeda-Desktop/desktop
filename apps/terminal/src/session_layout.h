#pragma once

#include "session_list.h"

#include <cstdint>
#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string>
#include <vector>

namespace term {

  struct SavedSession {
    std::string cwd; // empty: unknown, the shell starts in $HOME
    std::string name; // the user's name for the row; empty: automatic
    bool operator==(const SavedSession&) const = default;
  };

  // One window's shell sessions, for a restore after a reboot. splits and shown index sessions.
  struct SavedWindow {
    std::vector<SavedSession> sessions;
    std::vector<std::vector<std::size_t>> splits;
    std::size_t shown = 0;
    bool operator==(const SavedWindow&) const = default;
  };

  // Shell sessions only: an agent, ssh, container or app session does not start again from a folder.
  // cwdOf: the session's directory.
  [[nodiscard]] SavedWindow saveLayout(const SessionList& list, const std::function<std::string(std::uint64_t)>& cwdOf);
  // ids[i]: the open session made for saved.sessions[i]. The events give them the saved names, splits and shown session.
  [[nodiscard]] std::vector<SessionListEvent> restoreEvents(const SavedWindow& saved, std::span<const std::uint64_t> ids);

  [[nodiscard]] nlohmann::json toJson(const std::vector<SavedWindow>& windows);
  // Drops a window with no session; an index out of range drops its split or shows the first session.
  [[nodiscard]] std::vector<SavedWindow> windowsFromJson(const nlohmann::json& json);

} // namespace term
