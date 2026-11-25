#pragma once

#include "scene/surface_shadow.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct wlr_scene_tree;

namespace andromeda {
  class Server;

  // Modal confirmation panel. Rendering only: key and button handling live in
  // Keyboard::handleKey and Cursor::handleButton.
  class QuitConfirm {
  public:
    enum class Kind {
      SessionQuit,
      ScreenCastDynamic,
      InputEmulation,
      InputCapture,
      RemoteInput,
      GlobalShortcuts,
      LayerShell,
    };

    QuitConfirm(Server& server, wlr_scene_tree* parent);
    ~QuitConfirm();

    QuitConfirm(const QuitConfirm&) = delete;
    QuitConfirm& operator=(const QuitConfirm&) = delete;

    void show(Kind kind);
    void showInputEmulation(std::string_view application);
    void showInputCapture(std::string_view application, uint32_t persistMode);
    void showRemoteInput(std::string_view application, uint32_t persistMode);
    void showLayerShell(std::string_view application);
    // One page of "name  key" rows; page is 0-based.
    void showGlobalShortcuts(std::string_view application, std::vector<std::string> rows, size_t page, size_t pages);
    void hide();
    [[nodiscard]] bool visible() const { return m_tree != nullptr; }
    [[nodiscard]] Kind kind() const { return m_kind; }
    [[nodiscard]] bool canRemember() const {
      return m_kind == Kind::LayerShell
          || ((m_kind == Kind::InputCapture || m_kind == Kind::RemoteInput)
              && (m_inputCapturePersistMode == 1 || m_inputCapturePersistMode == 2));
    }
    void relayout(); // re-render if visible (output add/remove/scale change)

  private:
    void render();
    void destroyTree();

    Server& m_server;
    wlr_scene_tree* m_parent;
    wlr_scene_tree* m_tree = nullptr;
    Kind m_kind = Kind::SessionQuit;
    std::string m_application;
    uint32_t m_inputCapturePersistMode = 0;
    std::vector<std::string> m_rows;
    size_t m_page = 0;
    size_t m_pages = 1;
    SurfaceShadow m_shadow;
  };

} // namespace andromeda
