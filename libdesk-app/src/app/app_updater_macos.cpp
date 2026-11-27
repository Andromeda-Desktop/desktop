#include "app/app_updater.h"

#include "app/app_loop.h"
#include "app/app_update.h"
#include "core/fd_watch.h"
#include "core/log.h"
#include "core/sys/sys.h"
#include "core/timer_manager.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <optional>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {
  constexpr Logger kLog("app-update");
  constexpr auto kCheckInterval = std::chrono::hours(24);
  constexpr std::size_t kMaxOutput = 256;
  constexpr std::size_t kMaxVersion = 32;

  // `ready <version>` with a dotted numeric version; anything else counts as no update.
  std::string parseReady(const std::string& out) {
    constexpr std::string_view prefix = "ready ";
    if (!out.starts_with(prefix)) {
      return {};
    }
    std::string version = out.substr(prefix.size());
    while (!version.empty() && (version.back() == '\n' || version.back() == '\r')) {
      version.pop_back();
    }
    const bool valid = !version.empty() && version.size() <= kMaxVersion
        && std::ranges::all_of(version, [](char c) { return (c >= '0' && c <= '9') || c == '.'; });
    return valid ? version : std::string{};
  }

  class MacUpdater final : public AppUpdater {
  public:
    MacUpdater(AppLoop& loop, std::string helper, std::string manifestUrl,
               std::function<void(const std::string&)> onReady)
        : m_loop(loop), m_helper(std::move(helper)), m_url(std::move(manifestUrl)), m_onReady(std::move(onReady)) {
      m_timer.startRepeating(kCheckInterval, [this] { apply(UpdateEvent::CheckDue); });
      apply(UpdateEvent::CheckDue);
    }

    ~MacUpdater() override {
      if (m_phase == UpdatePhase::Checking) {
        stopCheck();
      }
    }

    bool restart() override {
      apply(UpdateEvent::Restart);
      return m_phase == UpdatePhase::HandedOff;
    }
    void quit() override { apply(UpdateEvent::Quit); }
    [[nodiscard]] std::optional<std::string> readyVersion() const override {
      return m_phase == UpdatePhase::Ready ? std::optional(m_version) : std::nullopt;
    }

  private:
    void apply(UpdateEvent event) {
      const UpdateStep step = updateStep(m_phase, event);
      m_phase = step.next;
      switch (step.action) {
      case UpdateAction::None:
        break;
      case UpdateAction::SpawnCheck:
        if (!spawnCheck()) {
          apply(UpdateEvent::CheckedNone);
          return;
        }
        break;
      case UpdateAction::ShowReady:
        m_onReady(m_version);
        break;
      case UpdateAction::SwapAndReopen:
      case UpdateAction::SwapOnExit:
        apply(spawnSwap(step.action == UpdateAction::SwapAndReopen) ? UpdateEvent::SwapStarted
                                                                     : UpdateEvent::SwapFailed);
        return;
      case UpdateAction::StopCheck:
        stopCheck();
        break;
      }
      if (m_phase == UpdatePhase::HandedOff) {
        m_timer.stop();
      }
    }

    bool spawnCheck() {
      int out[2];
      if (!sys::pipeCloexec(out)) {
        kLog.warn("update check: no pipe");
        return false;
      }
      const int null = ::open("/dev/null", O_RDWR | O_CLOEXEC);
      std::vector<std::string> env;
      for (char** e = environ; *e != nullptr; ++e) {
        env.emplace_back(*e);
      }
      const int pid = null < 0 ? -1 : sys::spawnInGroup(m_helper, {m_helper, "check", m_url}, env, {null, out[1], 2});
      ::close(out[1]);
      if (null >= 0) {
        ::close(null);
      }
      if (pid <= 0) {
        ::close(out[0]);
        kLog.warn("update check: cannot start {}", m_helper);
        return false;
      }
      m_child = pid;
      m_output.clear();
      m_watch = std::make_unique<FdWatch>(m_loop.pollLoop(), out[0], POLLIN, [this](short /*revents*/) { onOutput(); });
      return true;
    }

    void onOutput() {
      char buf[128];
      const ssize_t n = ::read(m_watch->fd(), buf, sizeof buf);
      if (n < 0 && errno == EINTR) {
        return;
      }
      if (n > 0) {
        m_output.append(buf, static_cast<std::size_t>(std::min<std::size_t>(static_cast<std::size_t>(n),
                                                                           kMaxOutput - m_output.size())));
        if (m_output.size() < kMaxOutput) {
          return;
        }
      }
      // Past kMaxOutput the child may still run: stop it, so the wait never holds the loop.
      const int status = closeChild(n != 0);
      m_version = WIFEXITED(status) && WEXITSTATUS(status) == 0 ? parseReady(m_output) : std::string{};
      if (m_version.empty()) {
        kLog.info("update check: {}", m_output.empty() ? "failed" : std::string_view(m_output).substr(0, 64));
        apply(UpdateEvent::CheckedNone);
      } else {
        kLog.info("update {} ready", m_version);
        apply(UpdateEvent::CheckedReady);
      }
    }

    // Closes the pipe and reaps the child; kill first stops one that still runs (its stage has no marker).
    int closeChild(bool kill) {
      const int fd = m_watch->fd();
      m_watch.reset();
      ::close(fd);
      if (kill) {
        ::kill(-m_child, SIGTERM);
      }
      int status = -1; // not WIFEXITED: a failed wait counts as a failed check
      while (::waitpid(m_child, &status, 0) < 0 && errno == EINTR) {
      }
      m_child = -1;
      return status;
    }

    void stopCheck() { closeChild(true); }

    bool spawnSwap(bool reopen) {
      std::vector<std::string> args{"swap", std::to_string(::getpid())};
      if (reopen) {
        args.emplace_back("reopen");
      }
      if (!sys::spawnDetached(m_helper, args)) {
        kLog.error("update swap: cannot start {}", m_helper);
        return false;
      }
      kLog.info("update {} installs after exit", m_version);
      return true;
    }

    AppLoop& m_loop;
    std::string m_helper;
    std::string m_url;
    std::function<void(const std::string&)> m_onReady;
    UpdatePhase m_phase = UpdatePhase::Idle;
    Timer m_timer;
    std::unique_ptr<FdWatch> m_watch; // the check child's stdout; set only while Checking
    int m_child = -1;
    std::string m_output;
    std::string m_version;
  };
} // namespace

std::unique_ptr<AppUpdater> makeAppUpdater(AppLoop& loop, const std::string& manifestUrl,
                                           std::function<void(const std::string&)> onReady) {
  if (manifestUrl.empty()) {
    return nullptr;
  }
  const std::filesystem::path exe = sys::executablePath();
  std::filesystem::path helper = exe.parent_path() / "andromeda-app-updater";
  std::error_code ec;
  if (exe.empty() || !std::filesystem::is_regular_file(helper, ec)) {
    kLog.info("no update helper beside {}", exe.string());
    return nullptr;
  }
  return std::make_unique<MacUpdater>(loop, helper.string(), manifestUrl, std::move(onReady));
}
