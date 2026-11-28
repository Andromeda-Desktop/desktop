// Store part of the theme switch model test (theme-engine STORES.md I1, I2, I5): random Settings commits and hand
// edits of overrides.toml (S2) and theme-overrides.toml (S4), theme folder edits (S5), watch events delivered late,
// and restarts. After the watches settle, the files, the config and the tokens equal an oracle that applies every
// event in order: no update is lost, a broken theme edit keeps the last good values.

#include "config/config_migrations.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/files/directory_watcher.h"
#include "core/files/file_watcher.h"
#include "core/timer_manager.h"
#include "design_tokens.h"
#include "protocols/async_file_writer.h"
#include "theme/desktop_theme.h"
#include "ui/token_slots.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <poll.h>
#include <print>
#include <random>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

  bool g_ok = true;
  std::string g_trace;

  void expect(bool condition, const std::string& message) {
    if (!condition && g_ok) {
      std::println(stderr, "theme_switch_model_test: FAIL: {}\ntrace:{}", message, g_trace);
      g_ok = false;
    }
  }

  void writeFile(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::trunc) << text;
  }

  // One rename event, as editors save.
  void writeFileAtomic(const std::filesystem::path& path, std::string_view text) {
    const auto temp = path.string() + ".tmp";
    writeFile(temp, text);
    std::filesystem::rename(temp, path);
  }

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
  }

  constexpr std::array<std::pair<WallpaperFillMode, std::string_view>, 3> kFills{{
      {WallpaperFillMode::Center, "center"}, {WallpaperFillMode::Fit, "fit"}, {WallpaperFillMode::Stretch, "stretch"}}};
  constexpr std::array<std::pair<ThemeMode, std::string_view>, 2> kModes{{{ThemeMode::Dark, "dark"},
                                                                           {ThemeMode::Light, "light"}}};

  // The disk as the events left it: S2 and S4 keys by index (absent: unset), and the theme corner. While theme.toml
  // is broken, the shell shows the last good corner it read: one of `themeSeen` (-1: the built-in value).
  struct Oracle {
    std::optional<int> fill;
    std::optional<int> mode;
    std::optional<int> windowCorner;
    std::optional<int> dockIcon;
    int themeCorner = 12; // theme.toml's corner while it is good
    bool themeBroken = false;
    std::set<int> themeSeen{12}; // the good corners written since the shell last surely read theme.toml
    // A Settings commit while an overrides.toml hand edit is unsettled waits for the watch reload, then lands on top.
    bool s2Unsettled = false;
    std::vector<std::pair<bool, int>> s2Queued; // (fill, index)

    void commitS2(bool isFill, int index) {
      (isFill ? fill : mode) = index;
    }
    void settleS2() {
      for (const auto& [isFill, index] : s2Queued) {
        commitS2(isFill, index);
      }
      s2Queued.clear();
      s2Unsettled = false;
    }
  };

  struct Paths {
    std::filesystem::path s2;
    std::filesystem::path s4;
    std::filesystem::path theme;
  };

  std::string s2Text(const Oracle& o) {
    std::string text = std::format("config_version = {}\n", noctalia::config::currentConfigVersion());
    if (o.mode) {
      text += std::format("[theme]\nmode = \"{}\"\n", kModes[*o.mode].second);
    }
    if (o.fill) {
      text += std::format("[wallpaper]\nfill_mode = \"{}\"\n", kFills[*o.fill].second);
    }
    return text;
  }

  std::string s4Text(const Oracle& o) {
    std::string text = "[values]\n";
    if (o.windowCorner) {
      text += std::format("\"corner.window\" = {}\n", *o.windowCorner);
    }
    if (o.dockIcon) {
      text += std::format("\"dock.icon\" = {}\n", *o.dockIcon);
    }
    return text;
  }

  std::string themeText(int corner) {
    return std::format("[theme]\nformat = 1\nid = \"deep\"\nname = \"Deep\"\n[values]\n\"dock.corner\" = {}\n", corner);
  }

  // Waits for the shell's queued write without delivering a watch event, so the edit lands after it on disk.
  void waitLanded(const std::filesystem::path& path) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (andromeda::pendingFileText(path) && std::chrono::steady_clock::now() < deadline) {
      ::poll(nullptr, 0, 2);
    }
  }

  struct Shell {
    DirectoryWatcher dirs;
    FileWatcher files{dirs};
    std::unique_ptr<noctalia::theme::DesktopTheme> theme;
    std::unique_ptr<ConfigService> config;

    // A new process: the token slots start built-in.
    void start() {
      config.reset();
      theme.reset();
      if (!desk::tokens::applySlotValues({})) {
        std::abort();
      }
      theme = std::make_unique<noctalia::theme::DesktopTheme>(files);
      config = std::make_unique<ConfigService>(&dirs);
      config->setTokenSource([this](const Config& c, const std::optional<std::string>& text) {
        return theme->sync(c.theme.desktopTheme, c.theme.accentSource == AccentSource::Custom, text);
      });
      theme->setChangeCallback([this] { config->reloadAllSections(); });
      config->forceReload();
    }

    // Delivers the queued watch events, runs deferred calls (FileWatcher callbacks) and due timers for `ms`, or until `done` holds.
    bool pump(int ms, const std::function<bool()>& done = {}) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
      while (std::chrono::steady_clock::now() < deadline) {
        pollfd descriptor{.fd = dirs.fd(), .events = POLLIN};
        if (::poll(&descriptor, 1, 5) > 0) {
          dirs.processEvents();
        }
        DeferredCall::drainWakeFd();
        for (auto& fn : DeferredCall::takePending()) {
          fn();
        }
        TimerManager::instance().tick();
        if (done && done()) {
          return true;
        }
      }
      return false;
    }
  };

  struct BuiltIn {
    float windowCorner;
    float dockIcon;
    float dockCorner;
    WallpaperFillMode fill;
    ThemeMode mode;
  };

  // Every file landed, and the files, the config and the tokens show the oracle.
  std::string mismatch(const Shell& shell, const Paths& paths, const Oracle& o, const BuiltIn& builtIn) {
    if (andromeda::pendingFileText(paths.s2) || andromeda::pendingFileText(paths.s4)) {
      return "a write is pending";
    }
    const std::string s2 = readFile(paths.s2);
    const std::string s4 = readFile(paths.s4);
    const auto has = [](const std::string& text, std::string_view needle) { return text.contains(needle); };
    // A commit of the default value removes the key instead of writing it.
    if (o.fill && kFills[*o.fill].first != builtIn.fill
        && !has(s2, std::format("fill_mode = \"{}\"", kFills[*o.fill].second))) {
      return std::format("overrides.toml lost fill_mode {}", kFills[*o.fill].second);
    }
    if (o.mode && kModes[*o.mode].first != builtIn.mode
        && !has(s2, std::format("mode = \"{}\"", kModes[*o.mode].second))) {
      return std::format("overrides.toml lost mode {}", kModes[*o.mode].second);
    }
    if (o.windowCorner && !has(s4, std::format("\"corner.window\" = {}", *o.windowCorner))) {
      return std::format("theme-overrides.toml lost corner.window {}", *o.windowCorner);
    }
    if (o.dockIcon && !has(s4, std::format("\"dock.icon\" = {}", *o.dockIcon))) {
      return std::format("theme-overrides.toml lost dock.icon {}", *o.dockIcon);
    }
    const Config& c = shell.config->config();
    if (c.wallpaper.fillMode != (o.fill ? kFills[*o.fill].first : builtIn.fill)) {
      return "config fill_mode differs";
    }
    if (c.theme.mode != (o.mode ? kModes[*o.mode].first : builtIn.mode)) {
      return "config theme mode differs";
    }
    if (Tokens::componentWindowRadius != (o.windowCorner ? static_cast<float>(*o.windowCorner) : builtIn.windowCorner)) {
      return std::format("token window radius {}, oracle {}", Tokens::componentWindowRadius, o.windowCorner.value_or(-1));
    }
    if (Tokens::componentDockIcon != (o.dockIcon ? static_cast<float>(*o.dockIcon) : builtIn.dockIcon)) {
      return std::format("token dock icon {}", Tokens::componentDockIcon);
    }
    const auto shows = [&](int corner) {
      return Tokens::componentDockRadius == (corner < 0 ? builtIn.dockCorner : static_cast<float>(corner));
    };
    if (o.themeBroken ? std::ranges::none_of(o.themeSeen, shows) : !shows(o.themeCorner)) {
      return std::format("token dock radius {} (theme {})", Tokens::componentDockRadius,
                         o.themeBroken ? -1 : o.themeCorner);
    }
    return {};
  }

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("noctalia-theme-switch-" + std::to_string(::getpid()));
  ::setenv("ANDROMEDA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("ANDROMEDA_STATE_HOME", (root / "state").c_str(), 1);
  ::setenv("ANDROMEDA_DATA_HOME", (root / "data").c_str(), 1);
  ::unsetenv("ANDROMEDA_DEV_TOKENS");
  const Paths paths{
      .s2 = root / "state" / "andromeda" / "shell" / "overrides.toml",
      .s4 = root / "config" / "andromeda" / "theme-overrides.toml",
      .theme = root / "data" / "andromeda" / "desktop-themes" / "deep" / "theme.toml",
  };
  const BuiltIn builtIn{
      .windowCorner = Tokens::componentWindowRadius,
      .dockIcon = Tokens::componentDockIcon,
      .dockCorner = Tokens::componentDockRadius,
      .fill = Config{}.wallpaper.fillMode,
      .mode = Config{}.theme.mode,
  };
  const std::string s4Root(ConfigService::kThemeOverridesRoot);

  std::mt19937 rng(20261128);
  std::uniform_int_distribution<int> pick(0, 99);
  for (int sequence = 0; sequence < 60 && g_ok; ++sequence) {
    std::filesystem::remove_all(root);
    writeFile(root / "config" / "andromeda" / "shell" / "config.toml", "[theme]\ndesktop_theme = \"deep\"\n");
    Oracle o;
    writeFile(paths.theme, themeText(12));
    g_trace = std::format(" sequence {}:", sequence);
    Shell shell;
    shell.start();

    for (int step = 0; step < 8 && g_ok; ++step) {
      const int roll = pick(rng);
      const int value = pick(rng);
      if (roll < 20) {
        const bool fill = value % 2 == 0;
        const int index = value % static_cast<int>(fill ? kFills.size() : kModes.size());
        if (fill) {
          shell.config->setOverride({"wallpaper", "fill_mode"}, std::string(kFills[index].second));
        } else {
          shell.config->setOverride({"theme", "mode"}, std::string(kModes[index].second));
        }
        if (o.s2Unsettled) {
          o.s2Queued.emplace_back(fill, index);
        } else {
          o.commitS2(fill, index);
        }
        g_trace += std::format(" commitS2({}={})", fill ? "fill" : "mode", index);
      } else if (roll < 35) {
        // A hand edit of the newest file: it waits for its watch event, which a later step delivers.
        waitLanded(paths.s2);
        if (value % 2 == 0) {
          o.fill = value % static_cast<int>(kFills.size());
        } else {
          o.mode = value % static_cast<int>(kModes.size());
        }
        writeFileAtomic(paths.s2, s2Text(o));
        o.s2Unsettled = true;
        g_trace += std::format(" editS2({}={})", value % 2 == 0 ? "fill" : "mode", value % 2 == 0 ? *o.fill : *o.mode);
      } else if (roll < 50) {
        const bool window = value % 2 == 0;
        if (window) {
          o.windowCorner = value % 33;
          shell.config->setOverride({s4Root, "corner.window"}, static_cast<std::int64_t>(*o.windowCorner));
        } else {
          o.dockIcon = 24 + value % 57;
          shell.config->setOverride({s4Root, "dock.icon"}, static_cast<std::int64_t>(*o.dockIcon));
        }
        g_trace += std::format(" commitS4({}={})", window ? "corner" : "icon", window ? *o.windowCorner : *o.dockIcon);
      } else if (roll < 62) {
        waitLanded(paths.s4);
        if (value % 2 == 0) {
          o.windowCorner = value % 33;
        } else {
          o.dockIcon = 24 + value % 57;
        }
        writeFileAtomic(paths.s4, s4Text(o));
        g_trace += std::format(" editS4({}={})", value % 2 == 0 ? "corner" : "icon",
                             value % 2 == 0 ? *o.windowCorner : *o.dockIcon);
      } else if (roll < 74) {
        o.themeBroken = value % 4 == 0;
        if (o.themeBroken) {
          writeFileAtomic(paths.theme, "[theme]\nformat = 9\n");
        } else {
          o.themeCorner = value % 41;
          o.themeSeen.insert(o.themeCorner);
          writeFileAtomic(paths.theme, themeText(o.themeCorner));
        }
        g_trace += std::format(" editTheme({})", o.themeBroken ? "broken" : std::format("{}", o.themeCorner));
      } else if (roll < 90) {
        // Past the 150 ms watch debounce, or so short that 8 of them stay inside it.
        const int ms = value % 3 == 0 ? 200 : value / 10;
        g_trace += std::format(" pump({})", ms);
        shell.pump(ms);
        if (ms >= 200) {
          o.settleS2();
        }
        if (ms > 0 && !o.themeBroken) {
          o.themeSeen = {o.themeCorner}; // the pump delivered the theme.toml event, and the shell read it
        }
      } else {
        g_trace += " restart";
        o.settleS2(); // the old shell writes its queue on exit
        o.themeSeen = {o.themeBroken ? -1 : o.themeCorner}; // no last good table survives a restart
        shell.start();
      }
    }
    g_trace += " settle";
    o.settleS2();
    std::string last;
    shell.pump(3000, [&] {
      last = mismatch(shell, paths, o, builtIn);
      return last.empty();
    });
    expect(last.empty(), std::format("{}\noverrides.toml:\n{}theme-overrides.toml:\n{}", last, readFile(paths.s2),
                                     readFile(paths.s4)));
  }
  std::filesystem::remove_all(root);
  if (!g_ok) {
    return 1;
  }
  std::println("theme_switch_model_test: ok");
  return 0;
}
