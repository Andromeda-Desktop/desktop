#pragma once

#include "protocols/dwindle_tree.h"
#include "session_list.h"

#include <cstdint>
#include <functional>
#include <optional>
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

  struct SavedSplit {
    std::vector<std::size_t> panes; // in layout order
    // The split's tree; a leaf key is its pane's session index + 1 (a tile key is never 0). nullopt: sides are picked anew.
    std::optional<std::vector<andromeda::tiling::DwindleTree::ShapeNode>> shape;
    bool operator==(const SavedSplit&) const = default;
  };

  // One window's shell sessions, for a restore after a reboot. splits and shown index sessions.
  struct SavedWindow {
    std::vector<SavedSession> sessions;
    std::vector<SavedSplit> splits;
    std::size_t shown = 0;
    std::optional<std::string> name; // the user's name for the window; nullopt: the shown session's name
    bool operator==(const SavedWindow&) const = default;
  };

  // Shell sessions only: an agent, ssh, container or app session does not start again from a folder.
  // cwdOf: the session's directory. treeOf: the split tree holding the session, or null.
  [[nodiscard]] SavedWindow saveLayout(const SessionList& list, const std::function<std::string(std::uint64_t)>& cwdOf,
                                       const std::function<const andromeda::tiling::DwindleTree*(std::uint64_t)>& treeOf);
  // ids[i]: the open session made for saved.sessions[i]. The events give them the saved names, splits and shown session.
  [[nodiscard]] std::vector<SessionListEvent> restoreEvents(const SavedWindow& saved, std::span<const std::uint64_t> ids);
  // The saved split trees over the sessions in ids, for the split view to adopt.
  [[nodiscard]] std::vector<andromeda::tiling::DwindleTree> restoreTrees(const SavedWindow& saved, std::span<const std::uint64_t> ids);

  [[nodiscard]] nlohmann::json toJson(const std::vector<SavedWindow>& windows);
  // Drops a window with no session; an index out of range drops its split or shows the first session; a shape that is
  // not a tree over exactly its split's panes is dropped.
  [[nodiscard]] std::vector<SavedWindow> windowsFromJson(const nlohmann::json& json);

} // namespace term
