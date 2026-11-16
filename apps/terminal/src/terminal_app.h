#pragma once

#include "app/desk_app.h"
#include "app/file_chooser.h"
#include "app/foreign_export.h"
#include "render/core/thumbnail_service.h"
#include "prefs_window.h"
#include "terminal_window.h"

#include <array>
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
    // "exec": parameter[0] is the start directory, the rest the command (andromeda-terminal -e).
    void action(const std::string& name, const std::vector<std::string>& parameter,
                const std::string& activationToken) override;
    // With no window yet, sessions that lost their terminal process come back first, all in one window; with none,
    // the windows saved at the last quit open again (sessions.restore).
    void activate(const std::string& activationToken) override;
    void openWindow(const std::string& activationToken);
    void openPreferences(const std::string& activationToken);
    void openPath(const std::filesystem::path& path); // in its default app, activated from the Preferences window
    void startup() override;
    void shutdown() override;
    void gpuLost() override;

  private:
    // Before the windows: their agent views close their sessions in it.
    std::array<std::unique_ptr<agent::Host>, agent::kKindCount> m_agents; // made on first use
    agent::Host& agentHost(agent::Kind kind);
    ThumbnailService m_thumbnails; // outlives the windows: their views release thumbnails
    SettingsStore m_settings;
    AppWindow* m_prefsWindow = nullptr;
    std::unique_ptr<PrefsWindow> m_prefs;
    std::unique_ptr<desk::ForeignExport> m_prefsExport; // parents the import dialog to Preferences; lives with it
    std::unique_ptr<desk::FileChooser> m_chooser;
    std::unique_ptr<desk::ForeignExport> m_saveExport; // parents a save dialog to its terminal window
    void chooseSaveFile(AppWindow& window, const std::string& suggested,
                        std::function<void(std::optional<std::filesystem::path>)> picked);
    std::map<AppWindow*, std::unique_ptr<TerminalWindow>> m_windows;
    std::unique_ptr<Notifier> m_notifier;
    std::vector<WindowOrigin> m_nextOrigins{NewShell{}}; // the sessions of the window createWindow() makes next
    // Live: each change saves the open windows. Kept: the app quits; its closing windows stay saved for the restore.
    enum class LayoutSave : std::uint8_t { Live, Kept };
    LayoutSave m_layoutSave = LayoutSave::Live;
    void saveLayouts();
  };

} // namespace term
