#include "terminal_app.h"
#include "theme_cli.h"
#include "theme_store.h"

#include "agent_host.h"
#include "holder_client.h"
#include "kitty_images.h"
#include "notifier.h"
#include "terminal_control.h"
#include "terminal_window.h"

#include "core/log.h"

#include <gio/gio.h>

#include <filesystem>
#include <memory>
#include <utility>

namespace term {

  namespace {
    constexpr Logger kLog("terminal");
  } // namespace

  TerminalApp::TerminalApp() : DeskApp({.id = "dev.andromeda.Terminal", .name = "Terminal", .sharedGl = false}) {
    KittyImages::installPngDecoder();
  }

  agent::Host& TerminalApp::agentHost(agent::Kind kind) {
    std::unique_ptr<agent::Host>& host = m_agents.at(static_cast<std::size_t>(kind));
    if (host == nullptr) {
      host = std::make_unique<agent::Host>(kind, [this](std::uint64_t key) {
        for (auto& [window, terminal] : m_windows) {
          terminal->agentChanged(key);
        }
      });
    }
    return *host;
  }

  TerminalApp::~TerminalApp() = default;

  void TerminalApp::startup() {
    loop().addSource(&m_thumbnails);
    exportControl();
  }

  void TerminalApp::gpuLost() { m_thumbnails.abandonGpuResources(); }

  AppWindow& TerminalApp::createWindow() {
    AppWindow& window = addWindow({.appId = info().id, .title = info().name, .width = 820, .height = 520, .sidebarWidth = 240.0F, .material = m_settings.settings().material == WindowMaterial::Glass ? AppWindow::Material::Glass : AppWindow::Material::Solid, .contentCard = true});
    if (m_notifier == nullptr) {
      m_notifier = std::make_unique<Notifier>(bus());
    }
    m_windows[&window] = std::make_unique<TerminalWindow>(loop(), window, *m_notifier, [this](agent::Kind kind) -> agent::Host& { return agentHost(kind); }, m_thumbnails, m_settings, std::exchange(m_nextOrigins, std::vector<WindowOrigin>{NewShell{}}),
                                                          [this, &window] { closeWindow(window); },
                                                          [this](LayoutSave save) { sessionsChanged(save); },
                                                          [this](const std::string& token, std::vector<WindowOrigin> origins) {
                                                            m_nextOrigins = std::move(origins);
                                                            openWindow(token);
                                                          },
                                                          [this](const std::string& token) { openPreferences(token); },
                                                          [this, &window](const std::string& suggested, auto picked) {
                                                            chooseSaveFile(window, suggested, std::move(picked));
                                                          });
    saveLayouts();
    return window;
  }

  void TerminalApp::windowClosed(AppWindow& window) {
    if (&window == m_prefsWindow) {
      m_prefs.reset();
      m_prefsExport.reset();
      m_prefsWindow = nullptr;
    }
    m_windows.erase(&window);
    saveLayouts();
  }

  void TerminalApp::shutdown() { m_layoutWrites = LayoutWrites::Kept; }

  void TerminalApp::sessionsChanged(LayoutSave save) {
    if (save == LayoutSave::Saved) {
      saveLayouts();
    }
    if (m_control != nullptr) {
      m_control->emitSignal("SessionsChanged").onInterface(control::kInterface);
    }
  }

  void TerminalApp::saveLayouts() {
    if (m_layoutWrites == LayoutWrites::Kept) {
      return;
    }
    std::vector<SavedWindow> windows;
    for (const auto& [window, terminal] : m_windows) {
      if (SavedWindow saved = terminal->layout(); !saved.sessions.empty()) {
        windows.push_back(std::move(saved));
      }
    }
    state().set("/terminal/windows", toJson(windows));
  }

  void TerminalApp::windowSurfaceLost(AppWindow& window) {
    if (auto it = m_windows.find(&window); it != m_windows.end()) {
      it->second->surfaceLost();
    }
  }

  void TerminalApp::openPath(const std::filesystem::path& path) {
    std::unique_ptr<GAppLaunchContext, void (*)(gpointer)> context(g_app_launch_context_new(), g_object_unref);
    if (m_prefsWindow != nullptr) {
      if (const std::string token = loop().wayland().requestActivationToken(m_prefsWindow->wlSurface()); !token.empty()) {
        g_app_launch_context_setenv(context.get(), "XDG_ACTIVATION_TOKEN", token.c_str());
      }
    }
    GError* error = nullptr;
    const std::string uri = "file://" + path.string();
    if (!g_app_info_launch_default_for_uri(uri.c_str(), context.get(), &error)) {
      kLog.warn("open {}: {}", uri, error != nullptr ? error->message : "?");
      g_clear_error(&error);
    }
  }

  // One Preferences window per app: a second Ctrl+, raises it.
  void TerminalApp::chooseSaveFile(AppWindow& window, const std::string& suggested,
                                   std::function<void(std::optional<std::filesystem::path>)> picked) {
    if (bus() == nullptr) {
      return;
    }
    if (m_chooser == nullptr) {
      m_chooser = std::make_unique<desk::FileChooser>(*bus());
    }
    if (m_saveExport == nullptr) {
      m_saveExport = std::make_unique<desk::ForeignExport>(loop().wayland().display());
    }
    m_saveExport->handle(window.wlSurface(), [this, suggested, picked = std::move(picked)](const std::string& parent) {
      m_chooser->saveFile(parent, "Save Scrollback", suggested, picked);
    });
  }

  void TerminalApp::openPreferences(const std::string& activationToken) {
    if (m_prefsWindow == nullptr) {
      m_prefsWindow = &addWindow({.appId = info().id, .title = "Preferences", .stateKey = "preferences", .width = 760,
                                  .height = 560, .minWidth = 560, .minHeight = 400});
      m_prefs = std::make_unique<PrefsWindow>(
          *m_prefsWindow, m_settings,
          PrefsWindow::ThemeActions{
              .createWithAgent =
                  [this] {
                    std::error_code ec;
                    std::filesystem::create_directories(themesDir(), ec);
                    m_nextOrigins = {NewAgent{.kind = agent::Kind::Claude, .cwd = themesDir().string(), .prompt = themeAgentPrompt()}};
                    openWindow(loop().wayland().requestActivationToken(m_prefsWindow->wlSurface()));
                  },
              .openFolder =
                  [this] {
                    std::error_code ec;
                    std::filesystem::create_directories(themesDir(), ec);
                    openPath(themesDir());
                  },
              .importPackage =
                  [this] {
                    if (bus() == nullptr) {
                      return;
                    }
                    if (m_chooser == nullptr) {
                      m_chooser = std::make_unique<desk::FileChooser>(*bus());
                    }
                    if (m_prefsExport == nullptr) {
                      m_prefsExport = std::make_unique<desk::ForeignExport>(loop().wayland().display());
                    }
                    m_prefsExport->handle(m_prefsWindow->wlSurface(), [this](const std::string& parent) {
                      if (m_prefs == nullptr) {
                        return;
                      }
                      m_chooser->chooseFolder(parent, "Import Theme", [this](std::optional<std::filesystem::path> folder) {
                        if (folder && m_prefs != nullptr) {
                          m_prefs->importTheme(*folder);
                        }
                      });
                    });
                  },
              .openPath = [this](const std::filesystem::path& path) { openPath(path); }});
      if (!m_prefsWindow->open()) {
        return;
      }
    }
    m_prefsWindow->activate(activationToken);
  }

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

  void TerminalApp::action(const std::string& name, const std::vector<std::string>& parameter,
                           const std::string& activationToken) {
    if (name != "exec" || parameter.size() < 2) {
      DeskApp::action(name, parameter, activationToken);
      return;
    }
    m_nextOrigins = {NewShell{.cwd = parameter.front(), .argv = {parameter.begin() + 1, parameter.end()}}};
    if (AppWindow& window = createWindow(); window.open()) {
      window.activate(activationToken);
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
    } else if (m_settings.settings().restoreSessions) {
      for (const SavedWindow& saved : windowsFromJson(state().get("/terminal/windows", nlohmann::json::array()))) {
        m_nextOrigins.clear();
        AppWindow& window = createWindow();
        m_windows.at(&window)->restore(saved);
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
