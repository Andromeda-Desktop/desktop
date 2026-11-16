#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

class AppLoop;
class AppWindow;
class Node;
class Renderer;
struct wl_surface;

namespace viewer {

  class ViewerWindow;

  // The documents of one window as tabs. It alone installs the window's content, key, layout and close callbacks and
  // shows the active tab; the strip appears with two or more tabs.
  class ViewerTabs {
  public:
    ViewerTabs(AppLoop& loop, AppWindow& window);
    ~ViewerTabs();
    ViewerTabs(const ViewerTabs&) = delete;
    ViewerTabs& operator=(const ViewerTabs&) = delete;

    // Opens `path` in a new tab after the active one and shows it.
    void add(const std::filesystem::path& path);
    [[nodiscard]] ViewerWindow* active() const;
    void select(std::size_t index);
    void next() { step(1); }
    void previous() { step(-1); }
    // Asks first when the tab has unsaved edits; the last tab closes the window.
    void closeActive();

  private:
    [[nodiscard]] std::unique_ptr<Node> build(Renderer& renderer, float width, float height);
    [[nodiscard]] std::unique_ptr<Node> buildStrip();
    [[nodiscard]] std::optional<std::size_t> firstUnsaved() const;
    // Asks for each tab with unsaved edits in turn, then closes the window.
    void continueClose();
    void remove(std::size_t index);
    void step(int direction);
    void updateTitle();
    // Files dropped on the window open as tabs.
    void registerDropTarget();

    AppLoop& m_loop;
    AppWindow& m_window;
    std::vector<std::unique_ptr<ViewerWindow>> m_tabs;
    std::size_t m_active = 0;
    // A window with no tab shows this never-opened one: the toolbar and the open-or-drop message.
    std::unique_ptr<ViewerWindow> m_blank;
    wl_surface* m_dropSurface = nullptr;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace viewer
