#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

struct wl_event_loop;
struct wlr_buffer;

namespace andromeda {
  class MainLoopWorker;
}

namespace andromeda::ov {

  // App icons for the overview's title capsules, by app id: the .desktop file's Icon= (or the app id itself) looked up
  // in the GTK icon theme, Adwaita and hicolor, drawn on a worker thread and kept for the session. One per server
  // (Server::appIcons), shared by every surface that shows app icons.
  class AppIcons {
  public:
    explicit AppIcons(wl_event_loop* loop);
    ~AppIcons();
    AppIcons(const AppIcons&) = delete;
    AppIcons& operator=(const AppIcons&) = delete;

    // The icon at `px` device pixels square, or null while it loads or when the app has none. The buffer stays owned
    // by this cache.
    [[nodiscard]] wlr_buffer* get(std::string_view appId, int px);

    // `changed` runs on the event loop each time an icon finished loading, until unsubscribe(id).
    [[nodiscard]] std::uint64_t subscribe(std::function<void()> changed);
    void unsubscribe(std::uint64_t id);

  private:
    // "<app id>@<px>" -> icon; null while loading or when the app has none.
    std::unordered_map<std::string, wlr_buffer*> m_entries;
    std::vector<std::pair<std::uint64_t, std::function<void()>>> m_listeners;
    std::uint64_t m_nextListener = 1;
    std::unique_ptr<MainLoopWorker> m_worker; // last: joined before the entries its results write go away
  };

} // namespace andromeda::ov
