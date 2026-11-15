#include "terminal_app.h"

#include "agent_host.h"
#include "holder_client.h"
#include "kitty_images.h"
#include "notifier.h"
#include "terminal_window.h"

#include <filesystem>
#include <utility>

namespace term {

  TerminalApp::TerminalApp() : DeskApp({.id = "dev.andromeda.Terminal", .name = "Terminal", .sharedGl = false}) {
    KittyImages::installPngDecoder();
    m_agents = std::make_unique<agent::Host>([this](std::uint64_t key) {
      for (auto& [window, terminal] : m_windows) {
        terminal->agentChanged(key);
      }
    });
  }

  TerminalApp::~TerminalApp() = default;

  void TerminalApp::startup() { loop().addSource(&m_thumbnails); }

  void TerminalApp::gpuLost() { m_thumbnails.abandonGpuResources(); }

  AppWindow& TerminalApp::createWindow() {
    AppWindow& window = addWindow({.appId = info().id, .title = info().name, .width = 820, .height = 520, .sidebarWidth = 240.0F, .sidebarMaterial = AppWindow::SidebarMaterial::Glass});
    if (m_notifier == nullptr) {
      m_notifier = std::make_unique<Notifier>(bus());
    }
    m_windows[&window] = std::make_unique<TerminalWindow>(loop(), window, *m_notifier, *m_agents, m_thumbnails, m_settings, std::exchange(m_nextOrigins, std::vector<WindowOrigin>{NewShell{}}),
                                                          [this, &window] { closeWindow(window); },
                                                          [this](const std::string& token) { openWindow(token); });
    return window;
  }

  void TerminalApp::windowClosed(AppWindow& window) { m_windows.erase(&window); }

  void TerminalApp::windowSurfaceLost(AppWindow& window) { m_windows.at(&window)->surfaceLost(); }

  void TerminalApp::open(const std::vector<std::string>& uris, const std::string& activationToken) {
    bool opened = false;
    for (const std::string& uri : uris) {
      std::error_code ec;
      const std::string path = pathFromFileUri(uri);
      if (!std::filesystem::is_directory(path, ec)) {
        continue;
      }
      m_nextOrigins = {NewShell{path}};
      AppWindow& window = createWindow();
      if (window.open()) {
        window.activate(activationToken);
        opened = true;
      }
    }
    if (!opened) {
      activate(activationToken);
    }
  }

  void TerminalApp::openWindow(const std::string& activationToken) {
    AppWindow& window = createWindow();
    if (window.open()) {
      window.activate(activationToken);
    }
  }

  // Each launch opens a window, as in other terminals; the dock raises a running window without a launch.
  void TerminalApp::activate(const std::string& activationToken) {
    if (!windows().empty()) {
      openWindow(activationToken);
      return;
    }
    bool reattached = false;
    std::vector<WindowOrigin> origins;
    for (const std::uint64_t id : holder::listOrphans()) {
      origins.emplace_back(Reattach{id});
    }
    if (!origins.empty()) {
      m_nextOrigins = std::move(origins);
      AppWindow& window = createWindow();
      if (window.open()) {
        window.activate(activationToken);
        reattached = true;
      }
    }
    if (!reattached) {
      DeskApp::activate(activationToken);
    }
  }

} // namespace term
