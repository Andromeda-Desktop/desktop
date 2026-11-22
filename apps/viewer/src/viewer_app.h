#pragma once

#include "app/desk_app.h"

#include <filesystem>
#include <map>
#include <memory>

class LinkShare;

namespace desk {
  class FileChooser;
}

namespace viewer {

  class ViewerTabs;
  class ViewerWindow;

  // andromeda-viewer: one process (DeskApp single instance); an opened file becomes a tab of the active window.
  class ViewerApp final : public DeskApp {
  public:
    static constexpr const char* kAppId = "dev.andromeda.Viewer";
    ViewerApp();
    ~ViewerApp() override;

  protected:
    void startup() override;
    void open(const std::vector<std::string>& uris, const std::string& activationToken) override;
    // "search" with one string: searches the active window (the same path as typing in its search field).
    void action(const std::string& name, const std::vector<std::string>& parameter, const std::string& token) override;
    AppWindow& createWindow() override;
    void windowClosed(AppWindow& window) override;

  private:
    void addCommands();
    [[nodiscard]] ViewerWindow* active() const;
    [[nodiscard]] ViewerTabs* activeTabs() const;
    void chooseFile();

    std::unique_ptr<desk::FileChooser> m_chooser;
    std::unique_ptr<LinkShare> m_linkShare; // null without a session bus

    std::map<AppWindow*, std::unique_ptr<ViewerTabs>> m_windows;
  };

} // namespace viewer
