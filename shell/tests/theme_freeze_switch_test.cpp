// Model test of ThemeService's look switch through the compositor frame freeze (dsk_shell_v1 freeze_frame). Random
// sequences of mode switches, frame_frozen answers (current, stale, duplicate), refused freezes and passing time check:
// I1 the look never changes while a freeze waits for its answer, except by its 400 ms timeout;
// I2 at most one freeze waits; I3 thaw only follows an answered or timed-out freeze;
// I4 once every answer and timer settled, the shown palette is the resolved target of the last switch.

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/files/directory_watcher.h"
#include "core/files/file_watcher.h"
#include "core/timer_manager.h"
#include "net/http_client.h"
#include "tests/test_check.h"
#include "theme/desktop_theme.h"
#include "theme/theme_service.h"
#include "ui/palette.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <optional>
#include <print>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

  struct Compositor {
    bool canFreeze = true;
    std::optional<std::uint32_t> waiting; // a freeze sent and not yet answered or timed out
    std::chrono::steady_clock::time_point sentAt;
    std::vector<std::uint32_t> answered; // serials frame_frozen reached, or that timed out
    bool delivering = false;
    bool ticking = false;
    int violations = 0;
  };

  void runDeferred() {
    for (auto& call : DeferredCall::takePending()) {
      call();
    }
  }

  void advance(Compositor& compositor, std::chrono::milliseconds by) {
    std::this_thread::sleep_for(by);
    compositor.ticking = true;
    TimerManager::instance().tick();
    compositor.ticking = false;
    runDeferred();
  }

  bool runSequence(unsigned seed) {
    std::mt19937 rng(seed);
    DirectoryWatcher dirs;
    FileWatcher files(dirs);
    noctalia::theme::DesktopTheme desktopTheme(files);
    ConfigService config;
    HttpClient http;
    noctalia::theme::ThemeService theme(config, http, desktopTheme);
    Compositor compositor;

    theme.setChangeCallback([&] {
      if (compositor.waiting.has_value() && !compositor.delivering && !compositor.ticking) {
        std::println("seed {}: I1 the look changed while freeze {} waits", seed, *compositor.waiting);
        ++compositor.violations;
      }
    });
    theme.setFrameFreeze(
        [&](std::uint32_t serial) {
          if (!compositor.canFreeze) {
            return false;
          }
          if (compositor.waiting.has_value()) {
            std::println("seed {}: I2 freeze {} while freeze {} waits", seed, serial, *compositor.waiting);
            ++compositor.violations;
          }
          compositor.waiting = serial;
          compositor.sentAt = std::chrono::steady_clock::now();
          return true;
        },
        [&](std::uint32_t serial) {
          if (std::ranges::find(compositor.answered, serial) == compositor.answered.end()) {
            std::println("seed {}: I3 thaw {} before its freeze was answered", seed, serial);
            ++compositor.violations;
          }
        }
    );
    theme.apply();
    runDeferred();

    const char* modes[] = {"light", "dark", ""};
    std::uint32_t sent = 0;
    for (int step = 0; step < 10; ++step) {
      // A freeze older than the shell's 400 ms timeout counts as answered: the shell may then apply.
      if (compositor.waiting.has_value() && std::chrono::steady_clock::now() - compositor.sentAt > std::chrono::milliseconds(380)) {
        compositor.answered.push_back(*compositor.waiting);
        compositor.waiting.reset();
      }
      switch (rng() % 5) {
      case 0:
      case 1:
        theme.setModeOverride(modes[rng() % 3]);
        runDeferred();
        break;
      case 2:
        if (compositor.waiting.has_value()) {
          const std::uint32_t serial = rng() % 4 == 0 && *compositor.waiting > 1 ? *compositor.waiting - 1 : *compositor.waiting;
          if (serial == *compositor.waiting) {
            compositor.answered.push_back(serial);
            compositor.waiting.reset();
          }
          compositor.delivering = true;
          theme.onFrameFrozen(serial);
          compositor.delivering = false;
          runDeferred();
        } else if (sent > 0) {
          theme.onFrameFrozen(sent); // a duplicate answer
        }
        break;
      case 3:
        compositor.canFreeze = !compositor.canFreeze;
        break;
      default:
        advance(compositor, std::chrono::milliseconds(rng() % 2 == 0 ? 30 : 150));
        break;
      }
      if (compositor.waiting.has_value()) {
        sent = *compositor.waiting;
      }
    }

    if (compositor.waiting.has_value()) {
      compositor.answered.push_back(*compositor.waiting);
      const std::uint32_t serial = *compositor.waiting;
      compositor.waiting.reset();
      compositor.delivering = true;
      theme.onFrameFrozen(serial);
      compositor.delivering = false;
    }
    for (int i = 0; i < 20 && TimerManager::instance().pollTimeoutMs() >= 0; ++i) {
      advance(compositor, std::chrono::milliseconds(std::max(TimerManager::instance().pollTimeoutMs(), 1)));
    }
    const ::Palette shown = palette;
    theme.apply();
    runDeferred();
    if (!(palette == shown)) {
      std::println("seed {}: I4 the settled palette is not the last switch's target", seed);
      ++compositor.violations;
    }
    return compositor.violations == 0;
  }

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("noctalia-theme-freeze-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "config" / "andromeda" / "shell");
  std::filesystem::create_directories(root / "state" / "andromeda" / "shell");
  std::filesystem::create_directories(root / "data");
  ::setenv("ANDROMEDA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("ANDROMEDA_STATE_HOME", (root / "state").c_str(), 1);
  ::setenv("ANDROMEDA_DATA_HOME", (root / "data").c_str(), 1);

  for (unsigned seed = 1; seed <= 16; ++seed) {
    TEST_CHECK(runSequence(seed));
  }
  std::filesystem::remove_all(root);
  return 0;
}
