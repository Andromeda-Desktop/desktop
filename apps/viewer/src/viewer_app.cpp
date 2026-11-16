#include "viewer_app.h"

#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "viewer_tabs.h"
#include "viewer_window.h"
#include "app/file_chooser.h"
#include "app/foreign_export.h"
#include "wayland/wayland_connection.h"

#include <gio/gio.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace viewer {

  namespace {
    constexpr Logger kLog("viewer");

    std::filesystem::path pathFromUri(const std::string& uri) {
      if (uri.find("://") == std::string::npos) {
        return std::filesystem::absolute(uri);
      }
      char* path = g_filename_from_uri(uri.c_str(), nullptr, nullptr);
      std::filesystem::path out = path != nullptr ? path : "";
      g_free(path);
      return out;
    }
  } // namespace

  ViewerApp::ViewerApp() : DeskApp({.id = "dev.andromeda.Viewer", .name = "Viewer"}) {}

  ViewerApp::~ViewerApp() = default;

  void ViewerApp::startup() {
    addCommands();
  }

  void ViewerApp::addCommands() {
    const auto add = [this](const char* id, const char* label, const char* group, std::uint32_t sym,
                            std::uint32_t mods, void (ViewerWindow::*run)(), std::uint32_t altSym = 0) {
      commands().add({.id = id, .label = label, .group = group,
                      .shortcut = sym != 0 ? std::optional<KeyChord>(KeyChord{sym, mods}) : std::nullopt,
                      .enabled = [this] { return active() != nullptr; },
                      .run = [this, run] {
                        if (ViewerWindow* w = active(); w != nullptr) {
                          (w->*run)();
                        }
                      },
                      .alternate = altSym != 0 ? std::optional<KeyChord>(KeyChord{altSym, mods}) : std::nullopt});
    };
    add("edit.find", "Find", "Edit", XKB_KEY_f, KeyMod::Ctrl, &ViewerWindow::focusSearch);
    add("edit.copy", "Copy", "Edit", XKB_KEY_c, KeyMod::Ctrl, &ViewerWindow::copySelection);
    add("edit.select-all", "Select All", "Edit", XKB_KEY_a, KeyMod::Ctrl, &ViewerWindow::selectAll);
    add("view.zoom-in", "Zoom In", "View", XKB_KEY_plus, KeyMod::Ctrl, &ViewerWindow::zoomIn, XKB_KEY_equal);
    add("view.zoom-out", "Zoom Out", "View", XKB_KEY_minus, KeyMod::Ctrl, &ViewerWindow::zoomOut);
    add("view.zoom-fit", "Fit Width", "View", XKB_KEY_0, KeyMod::Ctrl, &ViewerWindow::zoomFit);
    add("edit.undo", "Undo", "Edit", XKB_KEY_z, KeyMod::Ctrl, &ViewerWindow::undo);
    add("edit.redo", "Redo", "Edit", XKB_KEY_z, KeyMod::Ctrl | KeyMod::Shift, &ViewerWindow::redo);
    add("file.save", "Save", "File", XKB_KEY_s, KeyMod::Ctrl, &ViewerWindow::save);
    add("view.toggle-sidebar", "Show Sidebar", "View", 0, 0, &ViewerWindow::toggleSidebar);
    const auto tabs = [this](const char* id, const char* label, std::optional<KeyChord> key, void (ViewerTabs::*run)(),
                             std::optional<KeyChord> alternate = std::nullopt) {
      commands().add({.id = id, .label = label, .group = "Window", .shortcut = key,
                      .enabled = [this] { return activeTabs() != nullptr; },
                      .run = [this, run] {
                        if (ViewerTabs* t = activeTabs(); t != nullptr) {
                          (t->*run)();
                        }
                      },
                      .alternate = alternate});
    };
    // Ctrl+W closes the tab, the window only with its last tab; Close Window gives up Ctrl+W first: a held shortcut
    // cannot be taken.
    commands().add({.id = "window.close", .label = "Close Window", .group = "Window", .run = [this] {
                      if (AppWindow* window = activeWindow()) {
                        closeWindow(*window);
                      }
                    }});
    tabs("window.close-tab", "Close Tab", KeyChord{XKB_KEY_w, KeyMod::Ctrl}, &ViewerTabs::closeActive);
    tabs("view.next-tab", "Next Tab", KeyChord{XKB_KEY_Tab, KeyMod::Ctrl}, &ViewerTabs::next);
    tabs("view.previous-tab", "Previous Tab", KeyChord{XKB_KEY_ISO_Left_Tab, KeyMod::Ctrl | KeyMod::Shift},
         &ViewerTabs::previous, KeyChord{XKB_KEY_Tab, KeyMod::Ctrl | KeyMod::Shift});
    const auto open = [this](const char* id, const char* label, std::uint32_t sym) {
      commands().add({.id = id, .label = label, .group = "File", .shortcut = KeyChord{sym, KeyMod::Ctrl},
                      .run = [this] { chooseFile(); }});
    };
    open("file.open", "Open…", XKB_KEY_o);
    open("file.new-tab", "New Tab", XKB_KEY_t);
    commands().add({.id = "view.fullscreen", .label = "Full Screen", .group = "View",
                    .shortcut = KeyChord{.sym = XKB_KEY_F11, .modifiers = 0},
                    .enabled = [this] { return activeWindow() != nullptr; },
                    .run = [this] {
                      if (AppWindow* w = activeWindow(); w != nullptr) {
                        w->toggleFullscreen();
                      }
                    }});
    commands().add({.id = "view.leave-fullscreen", .label = "Leave Full Screen", .group = "View",
                    .shortcut = KeyChord{.sym = XKB_KEY_Escape, .modifiers = 0},
                    .enabled = [this] { return activeWindow() != nullptr && activeWindow()->fullscreen(); },
                    .run = [this] { activeWindow()->toggleFullscreen(); }});
  }

  ViewerTabs* ViewerApp::activeTabs() const {
    AppWindow* window = activeWindow();
    const auto it = window != nullptr ? m_windows.find(window) : m_windows.end();
    return it != m_windows.end() ? it->second.get() : nullptr;
  }

  ViewerWindow* ViewerApp::active() const {
    const ViewerTabs* tabs = activeTabs();
    return tabs != nullptr ? tabs->active() : nullptr;
  }

  void ViewerApp::chooseFile() {
    AppWindow* window = activeWindow();
    if (window == nullptr || bus() == nullptr) {
      return;
    }
    if (m_chooser == nullptr) {
      m_chooser = std::make_unique<desk::FileChooser>(*bus());
    }
    if (m_export == nullptr) {
      m_export = std::make_unique<desk::ForeignExport>(loop().wayland().display());
    }
    m_export->handle(window->wlSurface(), [this, window](const std::string& parent) {
      m_chooser->openFile(parent, "Open", [this, window](std::optional<std::filesystem::path> path) {
        // The window may have closed while the dialog showed.
        if (const auto it = m_windows.find(window); path && it != m_windows.end()) {
          it->second->add(*path);
        }
      });
    });
  }

  AppWindow& ViewerApp::createWindow() {
    AppWindow& window = addWindow({.appId = info().id, .title = info().name, .width = 900, .height = 760,
                                       // The PDF toolbar's buttons, zoom and search field fit from this width.
                                       .minWidth = 720,
                                       .headerWhenFullscreen = false});
    m_windows[&window] = std::make_unique<ViewerTabs>(loop(), window);
    return window;
  }

  void ViewerApp::open(const std::vector<std::string>& uris, const std::string& activationToken) {
    for (const std::string& uri : uris) {
      const auto path = pathFromUri(uri);
      if (path.empty()) {
        kLog.warn("cannot open {}", uri);
        continue;
      }
      // A file opens as a tab of the active window, else of any open one, else of a new window.
      AppWindow* window = activeWindow();
      if (window == nullptr && !m_windows.empty()) {
        window = m_windows.begin()->first;
      }
      const bool fresh = window == nullptr;
      if (fresh) {
        window = &createWindow();
      }
      m_windows[window]->add(path);
      if (fresh) {
        window->open();
      }
      window->activate(activationToken);
    }
  }

  void ViewerApp::action(const std::string& name, const std::vector<std::string>& parameter, const std::string& token) {
    if (name == "search" && parameter.size() == 1) {
      if (ViewerWindow* w = active(); w != nullptr) {
        w->searchFor(parameter.front());
      }
      return;
    }
    DeskApp::action(name, parameter, token);
  }

  void ViewerApp::windowClosed(AppWindow& window) { m_windows.erase(&window); }

} // namespace viewer
