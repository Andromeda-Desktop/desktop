#pragma once

#include "app/color_scheme.h"
#include "app/app_clipboard.h"
#include "app/app_lifecycle.h"
#include "app/app_loop.h"
#include "app/app_services.h"
#include "app/app_updater.h"
#include "app/app_window.h"
#include "app/command_table.h"
#include "app/state_store.h"
#include "core/graphics_recovery.h"
#include "core/memory_pressure_poll_source.h"
#include "core/timer_manager.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class GlibSource;
class SessionBus;

// A first-party app process: run loop, session bus, single instance, state and the command table, with the
// standard window commands (window.close Ctrl+W, window.maximize, window.minimize, app.quit Ctrl+Q).
// Subclass it and implement activate() and open(); call run() from main().
class DeskApp {
public:
  struct Info {
    std::string id;   // reverse-DNS app id and D-Bus name, e.g. "dev.andromeda.Files"
    std::string name; // shown in the window title
    bool singleInstance = true; // false: every launch runs alone and claims no D-Bus name (a per-request helper)
    // false: no GL context for texture caches (loop().gl()); saves a Mesa context (~1 MB) and its thread.
    bool sharedGl = true;
    // The self-update manifest URL (macOS bundles with the update helper); empty: no update checks.
    std::string updateManifest;
  };

  explicit DeskApp(Info info);
  virtual ~DeskApp();

  DeskApp(const DeskApp&) = delete;
  DeskApp& operator=(const DeskApp&) = delete;

  // Arguments after argv[0] that are not options are files or URIs to open. Returns the process exit code.
  int run(int argc, char* argv[]);
  // Call first in main(), before the app object exists: when the app already runs, it gets this launch and the
  // exit code returns, so a second launch builds no app members. Empty when this process must start the app.
  static std::optional<int> forwardToRunning(const std::string& appId, int argc, char* argv[]);
  // --zygote: preloads the fonts of this app's last run, then forks one child per launch request. Returns only in the
  // child, with the argv of its request; the caller continues its normal main path with it.
  // The font pairs each app recorded at its last exit, one file per app id; andromeda-appd preloads them all.
  static std::filesystem::path fontWarmupDir();
  // Ends the app now, whatever windows or leases it holds.
  void quit(int code = 0);
  // Quits once no window is open and no lease is held; a window open ignores it.
  void quitWhenIdle() { onLifecycle(LifecycleEvent::Expired); }
  // Light or Dark for this app's windows only; System follows the desktop. Not stored: the app keeps its choice.
  void setColorScheme(ColorScheme scheme);

  // Work that outlives the windows (a file copy): while one is held, a hidden or windowless app does not quit on
  // expiry, but waits for the last one to end. Safe to destroy after the app.
  class Lease {
  public:
    Lease() = default;
    Lease(Lease&& other) noexcept = default;
    Lease& operator=(Lease&& other) noexcept {
      if (this != &other) {
        release();
        m_app = std::move(other.m_app);
      }
      return *this;
    }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    ~Lease() { release(); }

  private:
    friend class DeskApp;
    explicit Lease(std::weak_ptr<DeskApp*> app) : m_app(std::move(app)) {}
    void release() {
      if (const auto app = m_app.lock()) {
        m_app.reset();
        (*app)->releaseLease();
      }
    }
    std::weak_ptr<DeskApp*> m_app;
  };
  [[nodiscard]] Lease lease();

  [[nodiscard]] const Info& info() const noexcept { return m_info; }
  [[nodiscard]] AppLoop& loop() noexcept { return m_loop; }
  [[nodiscard]] CommandTable& commands() noexcept { return m_commands; }
  [[nodiscard]] StateStore& state() noexcept { return m_state; }
  // Null when the session bus is not reachable.
  [[nodiscard]] SessionBus* bus() noexcept { return m_services != nullptr ? m_services->bus() : nullptr; }
  // Shows the files (absolute paths), selected, in the desktop's file manager.
  void showInFolder(const std::vector<std::string>& paths) {
    if (m_services != nullptr) m_services->showInFolder(paths);
  }

  // A new window that the app owns; it is destroyed after its close, and the app quits after the last one.
  AppWindow& addWindow(AppWindow::Config config);
  [[nodiscard]] const std::vector<std::unique_ptr<AppWindow>>& windows() const noexcept { return m_windows; }
  // The window with keyboard focus, else the newest one; null when none is open.
  [[nodiscard]] AppWindow* activeWindow() const;
  // Closes the window after the current dispatch (safe from its own input callbacks).
  void closeWindow(AppWindow& window);

protected:
  // A launch with no files (first start or a forwarded launch). Default: raise the active window, else open one.
  virtual void activate(const std::string& activationToken);
  virtual void open(const std::vector<std::string>& uris, const std::string& activationToken);
  // org.freedesktop.Application.ActivateAction: runs the command with that id unless a subclass takes it first.
  virtual void action(const std::string& name, const std::vector<std::string>& parameter, const std::string& activationToken);
  // Called once before the first activate/open, with the loop and bus up.
  virtual void startup() {}
  // After the loop ends (any quit), before the state is flushed.
  virtual void shutdown() {}
  [[nodiscard]] virtual AppWindow& createWindow() = 0;
  // Before a closed window is destroyed: drop what refers to it.
  virtual void windowClosed(AppWindow& /*window*/) {}
  // The GL context died (compositor gone or a GPU reset): forget texture ids held outside window scenes; do not free
  // them.
  virtual void gpuLost() {}
  // The window's surface and scene are gone for a compositor reconnect; it opens again with a new surface.
  virtual void windowSurfaceLost(AppWindow& /*window*/) {}
  // False for a service process that outlives its windows (the file picker); it then calls quit() itself.
  [[nodiscard]] virtual bool quitWithLastWindow() const { return true; }
  // True: when the last window closes, the app stays alive and hidden, so a reopen skips the process start. It quits
  // after an hour hidden or when the system runs short of memory, once no lease is held.
  [[nodiscard]] virtual bool hideWhenClosed() const { return false; }
  // For a subclass destructor whose members the windows' content uses: destroys them first, with no windowClosed.
  void destroyWindows() { m_windows.clear(); }

private:
  // Everything before the loop runs; an exit code when the process must end without the loop.
  std::optional<int> start(int argc, char* argv[]);
  void addStandardCommands();
  void routePointer(const PointerEvent& event);
  void routeKey(const KeyboardEvent& event);
  [[nodiscard]] AppWindow* windowFor(const Surface* surface) const;
  void reapClosed();
  // Puts the Restart row in the window's header while a verified update waits.
  void showUpdate(AppWindow& window);
  void onLifecycle(LifecycleEvent event);
  void releaseLease();
  // Reopens every window on the compositor that took over the socket; false when none did.
  bool reconnect();
  // Rebuilds GL after a context reset and every window's scene, so textures upload again.
  void recoverGraphics();
  // Runs a launch request now, or after startup() when it arrives before (a D-Bus call queued at activation).
  void whenStarted(std::function<void()> request);

  enum class Phase { Starting, Started };
  Phase m_phase = Phase::Starting;
  std::vector<std::function<void()>> m_earlyRequests; // launch requests held while Starting
  Info m_info;
  AppLoop m_loop;
  GraphicsRecovery m_graphicsRecovery{[this] { recoverGraphics(); }};
  std::unique_ptr<AppServices> m_services;
  std::unique_ptr<PollSource> m_signals;
  std::unique_ptr<GlibSource> m_glib; // GIO async calls complete in the app loop
  std::unique_ptr<AppClipboard> m_clipboard;
  std::unique_ptr<class TextInputService> m_textInput;
  StateStore m_state;
  CommandTable m_commands;
  std::vector<std::unique_ptr<AppWindow>> m_windows;
  std::unique_ptr<AppUpdater> m_updater; // after m_loop: its timer and pipe watch leave the loop first
  Timer m_serviceQuit; // a D-Bus activation start whose request never came
  Lifecycle m_lifecycle = Lifecycle::Active;
  Timer m_hiddenQuit;
  std::unique_ptr<MemoryPressurePollSource> m_hiddenPressure; // armed while Hidden; kept until the loop ends after it
  int m_leases = 0;
  int m_exitCode = 0;
  Surface* m_pointerSurface = nullptr;
  // Last member: destroyed first, so a lease released while members die does not reach the app.
  std::shared_ptr<DeskApp*> m_self = std::make_shared<DeskApp*>(this);
};
