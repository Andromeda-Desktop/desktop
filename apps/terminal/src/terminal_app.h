#pragma once

#include "app/desk_app.h"
#include "app/file_chooser.h"
#include "render/core/thumbnail_service.h"
#include "prefs_window.h"
#include "terminal_window.h"
#include "workspace_watcher.h"

#include <sdbus-c++/sdbus-c++.h>

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
    std::unique_ptr<desk::FileChooser> m_chooser;
    void chooseSaveFile(AppWindow& window, const std::string& suggested,
                        std::function<void(std::optional<std::filesystem::path>)> picked);
    std::map<AppWindow*, std::unique_ptr<TerminalWindow>> m_windows;
    WorkspaceWatcher m_workspaces;
    // A window's workspace: Pending until the holder answers its PutWorkspace; absent until its first shell starts.
    struct WorkspacePending {
      std::uint32_t req;
      std::vector<std::uint64_t> held; // shells to move into it on the answer
    };
    std::map<AppWindow*, std::variant<WorkspacePending, std::uint64_t>> m_windowWorkspaces;
    void joinWorkspace(AppWindow& window, std::uint64_t heldId, const std::string& dir);
    void workspacesChanged(); // updates every window's foot bar
    // A foot-bar click: a workspace another window shows raises that window; another one moves into this window.
    void pickWorkspace(AppWindow& from, std::uint64_t id);
    void addWorkspace(AppWindow& from); // a new window, which is a new workspace
    // The window lets its shells go (they keep running, detached) and shows `origins` in workspace `next` (0: new).
    void leaveWorkspace(AppWindow& window, std::uint64_t next, std::vector<WindowOrigin> origins);
    std::unique_ptr<Notifier> m_notifier;
    std::vector<WindowOrigin> m_nextOrigins{NewShell{}}; // the sessions of the window createWindow() makes next
    // Live: each change saves the open windows. Kept: the app quits; its closing windows stay saved for the restore.
    enum class LayoutWrites : std::uint8_t { Live, Kept };
    LayoutWrites m_layoutWrites = LayoutWrites::Live;
    void saveLayouts();
    void sessionsChanged(LayoutSave save);

    // org.andromeda.Terminal1 at kControlPath (terminal_control.cpp): `andromeda-terminal ctl` and other clients.
    std::unique_ptr<sdbus::IObject> m_control;
    void exportControl();
    std::uint64_t controlOpen(const std::map<std::string, sdbus::Variant>& options);
    // The session's window; throws the D-Bus error NoSession.
    TerminalWindow& controlWindow(std::uint64_t id);
    TerminalView& controlTerminal(std::uint64_t id); // throws NotTerminal for an agent session
  };

} // namespace term
