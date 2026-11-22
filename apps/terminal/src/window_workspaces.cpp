#include "window_workspaces.h"

#include <filesystem>

namespace term {

  void WindowWorkspaces::opened(Window window, std::optional<std::uint64_t> workspace) {
    m_windows.insert_or_assign(window, workspace ? State{Joined{*workspace}} : State{Unjoined{}});
  }

  void WindowWorkspaces::sessionStarted(Window window, std::uint64_t held, const std::string& dir) {
    const auto it = m_windows.find(window);
    if (it == m_windows.end()) return;
    if (auto* joined = std::get_if<Joined>(&it->second)) {
      m_port.setSession(held, joined->id);
    } else if (auto* joining = std::get_if<Joining>(&it->second)) {
      joining->held.push_back(held);
    } else {
      auto waiting = std::get<Unjoined>(it->second).held;
      waiting.push_back(held);
      const std::filesystem::path root(dir);
      const std::string name = root.filename().empty() ? dir : root.filename().string();
      if (const auto req = m_port.put(name, dir)) {
        it->second = Joining{*req, std::move(waiting)};
      } else {
        it->second = Unjoined{std::move(waiting)};
      }
    }
  }

  void WindowWorkspaces::answered(std::uint32_t req, std::uint64_t id) {
    for (auto& [window, state] : m_windows) {
      if (auto* joining = std::get_if<Joining>(&state); joining != nullptr && joining->req == req) {
        for (const std::uint64_t held : joining->held) m_port.setSession(held, id);
        state = Joined{id};
        return;
      }
    }
    m_port.close(id); // its window closed while it was being made
  }

  void WindowWorkspaces::lost(std::uint32_t req) {
    for (auto& [window, state] : m_windows) {
      if (auto* joining = std::get_if<Joining>(&state); joining != nullptr && joining->req == req) {
        state = Unjoined{std::move(joining->held)};
        return;
      }
    }
  }

  void WindowWorkspaces::closed(Window window) {
    const auto it = m_windows.find(window);
    if (it == m_windows.end()) return;
    if (const auto* joined = std::get_if<Joined>(&it->second)) m_port.close(joined->id);
    m_windows.erase(it);
  }

  std::optional<std::uint64_t> WindowWorkspaces::workspaceOf(Window window) const {
    const auto it = m_windows.find(window);
    if (it == m_windows.end()) return std::nullopt;
    if (const auto* joined = std::get_if<Joined>(&it->second)) return joined->id;
    return std::nullopt;
  }

} // namespace term
