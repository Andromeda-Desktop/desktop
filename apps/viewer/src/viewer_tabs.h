#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "app/split_view.h"
#include "tab_set.h"
#include "viewer_window.h"

class AppLoop;
class AppWindow;
class Node;
class Renderer;
struct wl_surface;

class StateStore;

namespace viewer {

  class ViewerWindow;

  // The documents of one window as tabs. It alone installs the window's content, caption, sidebar, key, layout and close
  // callbacks and shows the active tab; the sidebar lists the documents with two or more.
  class ViewerTabs {
  public:
    ViewerTabs(AppLoop& loop, AppWindow& window, StateStore& state);
    ~ViewerTabs();
    ViewerTabs(const ViewerTabs&) = delete;
    ViewerTabs& operator=(const ViewerTabs&) = delete;

    // Opens `path` in a new tab after the active one and shows it.
    void add(const std::filesystem::path& path);
    [[nodiscard]] ViewerWindow* active() const;
    void select(std::uint64_t id);
    void next() { step(1); }
    void previous() { step(-1); }
    // Focus moves to the split pane nearest in direction (dx, dy).
    void focusPane(float dx, float dy);
    // Asks first when the tab has unsaved edits; the last tab closes the window.
    void closeActive();

  private:
    [[nodiscard]] std::unique_ptr<Node> build(Renderer& renderer, float width, float height);
    [[nodiscard]] std::unique_ptr<Node> buildSidebar(Renderer& renderer, float width);
    [[nodiscard]] std::unique_ptr<Node> buildDocuments(float width);
    // Full screen hides the sidebar; leaving it brings back the user's choice.
    void syncImmersive();
    [[nodiscard]] std::optional<std::uint64_t> firstUnsaved() const;
    // Asks for each tab with unsaved edits in turn, then closes the window.
    void continueClose();
    void remove(std::uint64_t id);
    [[nodiscard]] std::optional<std::uint64_t> idOf(const ViewerWindow* tab) const;
    void step(int direction);
    void updateTitle();
    // After a tab event: title, the window's frame (cards while a split shows) and a rebuild.
    void changed();
    // Ctrl+click: a free row joins the shown document's split, a member leaves it; else the row shows.
    void clickRow(std::uint64_t id);
    // `id` may join the focused document's split: it is free, not focused, and the split has room.
    [[nodiscard]] bool canJoin(std::uint64_t id) const;
    // `id` joins the focused document's split, or the half of a pane it was dropped on.
    void join(std::uint64_t id, const std::optional<ui::SplitDrop>& drop);
    void leave(std::uint64_t id);
    // One pane's pages rebuild in place; the whole window when `id` shows alone.
    void rebuildPane(std::uint64_t id);
    // rebuildPane once, after the current event.
    void schedulePane(std::uint64_t id);
    // A Documents row dragged to the scene point: the drop preview, and on drop the join.
    void rowDrag(std::uint64_t id, bool drop, float sceneX, float sceneY);
    void openRowMenu(std::uint64_t id, float sceneX, float sceneY);
    [[nodiscard]] SplitView::Callbacks splitCallbacks();
    // Files dropped on the window open as tabs.
    void registerDropTarget();

    AppLoop& m_loop;
    AppWindow& m_window;
    StateStore& m_state;
    ViewerWindow::RenderBudget m_budget; // before the documents: they give their jobs back on destruction
    TabSet m_set;
    std::unordered_map<std::uint64_t, std::unique_ptr<ViewerWindow>> m_docs;
    std::uint64_t m_nextId = 1;
    SplitView m_split;
    // A window with no tab shows this never-opened one: the toolbar and the open-or-drop message.
    std::unique_ptr<ViewerWindow> m_blank;
    Node* m_caption = nullptr; // the header accessory of the last build
    wl_surface* m_dropSurface = nullptr;
    bool m_immersive = false;
    std::unordered_set<std::uint64_t> m_pendingPanes; // panes that rebuild after the current event
    bool m_dragGhost = false; // a dragged Documents row shows its ghost
    float m_rowWidth = 0.0F;  // a Documents row's width, for its drag ghost
    bool m_allDocuments = false; // the Documents group shows every row, not the first 3
    bool m_sidebarBeforeImmersive = true;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace viewer
