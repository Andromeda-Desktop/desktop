#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace term {

  // Each open window owns one holder workspace, and every session of the window belongs to it. The one place that
  // changes the window to workspace map; the port sends its effects to the holder.
  class WindowWorkspaces {
  public:
    using Window = const void*;
    struct Port {
      // PutWorkspace for a new workspace: the request id, or nullopt when the holder is unreachable.
      std::function<std::optional<std::uint32_t>(const std::string& name, const std::string& root)> put;
      std::function<void(std::uint64_t held, std::uint64_t workspace)> setSession;
      std::function<void(std::uint64_t workspace)> close; // ends the workspace's shells too
    };

    struct Unjoined {
      std::vector<std::uint64_t> held; // sessions that started while the holder was unreachable
    };
    struct Joining {
      std::uint32_t req;
      std::vector<std::uint64_t> held; // sessions to move into it on the answer
    };
    struct Joined {
      std::uint64_t id;
    };
    using State = std::variant<Unjoined, Joining, Joined>;

    explicit WindowWorkspaces(Port port) : m_port(std::move(port)) {}

    // workspace: the one the window's sessions already belong to (sessions back from a crash).
    void opened(Window window, std::optional<std::uint64_t> workspace);
    // A session with a held id now shows in the window; dir names a new workspace.
    void sessionStarted(Window window, std::uint64_t held, const std::string& dir);
    void answered(std::uint32_t req, std::uint64_t id);
    void closed(Window window);

    [[nodiscard]] std::optional<std::uint64_t> workspaceOf(Window window) const;
    [[nodiscard]] const std::map<Window, State>& windows() const { return m_windows; }

  private:
    Port m_port;
    std::map<Window, State> m_windows;
  };

} // namespace term
