#pragma once

#include "app/desk_app.h"
#include "app/file_chooser.h"
#include "app/foreign_export.h"
#include "render/core/thumbnail_service.h"
#include "prefs_window.h"
#include "terminal_window.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace term {

  class Notifier;

  // andromeda-terminal: one process; each window runs one shell.
  class TerminalApp final : public DeskApp {
  public:
    TerminalApp();
    ~TerminalApp() override;

  protected:
    AppWindow& createWindow() override;
    void windowClosed(AppWindow& window) override;
    void windowSurfaceLost(AppWindow& window) override;
    // Each directory among the files opens a window there (andromeda-terminal <dir>, Files "Open Terminal Here").
    void open(const std::vector<std::string>& uris, const std::string& activationToken) override;
    // With no window yet, sessions that lost their terminal process come back first, one window each.
    void activate(const std::string& activationToken) override;
    void openWindow(const std::string& activationToken);
    void openPreferences(const std::string& activationToken);
    void openPath(const std::filesystem::path& path); // in its default app, activated from the Preferences window
    void startup() override;
    void gpuLost() override;

  private:
    // Before the windows: their agent views close their sessions in it.
    std::unique_ptr<agent::Host> m_agents;
    ThumbnailService m_thumbnails; // outlives the windows: their views release thumbnails
    SettingsStore m_settings;
    AppWindow* m_prefsWindow = nullptr;
    std::unique_ptr<PrefsWindow> m_prefs;
    std::unique_ptr<desk::ForeignExport> m_prefsExport; // parents the import dialog to Preferences; lives with it
    std::unique_ptr<desk::FileChooser> m_chooser;
    std::map<AppWindow*, std::unique_ptr<TerminalWindow>> m_windows;
    std::unique_ptr<Notifier> m_notifier;
    std::vector<WindowOrigin> m_nextOrigins{NewShell{}}; // the sessions of the window createWindow() makes next
  };

} // namespace term
