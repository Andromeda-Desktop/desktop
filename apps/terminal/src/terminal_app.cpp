#include "terminal_app.h"

#include "holder_client.h"
#include "kitty_images.h"
#include "notifier.h"
#include "terminal_window.h"

#include <filesystem>
#include <utility>

namespace term {

  TerminalApp::TerminalApp() : DeskApp({.id = "dev.andromeda.Terminal", .name = "Terminal", .sharedGl = false}) {
    KittyImages::installPngDecoder();
  }

  TerminalApp::~TerminalApp() = default;

  AppWindow& TerminalApp::createWindow() {
    AppWindow& window = addWindow({.appId = info().id, .title = info().name, .width = 820, .height = 520});
    if (m_notifier == nullptr) {
      m_notifier = std::make_unique<Notifier>(bus());
    }
    m_windows[&window] = std::make_unique<TerminalWindow>(loop(), window, *m_notifier, std::exchange(m_nextOrigin, NewShell{}),
                                                          [this, &window] { closeWindow(window); });
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
      m_nextOrigin = NewShell{path};
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

  void TerminalApp::activate(const std::string& activationToken) {
    bool reattached = false;
    if (windows().empty()) {
      for (const std::uint64_t id : holder::listOrphans()) {
        m_nextOrigin = Reattach{id};
        AppWindow& window = createWindow();
        if (window.open()) {
          window.activate(activationToken);
          reattached = true;
        }
      }
    }
    if (!reattached) {
      DeskApp::activate(activationToken);
    }
  }

} // namespace term
