// Desktop Layout picks replayed as Settings commits them: a snapshot of the current config, then the pick's writes
// and clears in one commit (SettingsWindow::mutateSettingOverrides). Every sequence of up to three picks must leave
// the bar a pure function of the last pick, so a pick applies in one tap and leaving the Windows or macOS row gives
// the user's own bar back.

#include "config/config_service.h"
#include "config/config_types.h"
#include "shell/desktop_layout/desktop_layout.h"

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <print>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

  int g_failures = 0;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::println(stderr, "desktop_layout_switch: FAIL: {}", message);
      ++g_failures;
    }
  }

  void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << content;
  }

  std::string join(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
      out += name + ",";
    }
    return out;
  }

  std::string barState(const Config& cfg) {
    const BarConfig& bar = cfg.bars.front();
    return std::format(
        "islands={} position={} dock={} thickness={} margin_ends={} margin_edge={} padding={} padding_end={} "
        "spacing={} start=[{}] center=[{}] end=[{}]",
        bar.islands, bar.position, cfg.dock.enabled, bar.thickness, bar.marginEnds, bar.marginEdge, bar.padding,
        bar.paddingEnd, bar.widgetSpacing, join(bar.startWidgets), join(bar.centerWidgets), join(bar.endWidgets)
    );
  }

  // Settings › Desktop Layout: compositor keys go over IPC, so only the shell's own keys reach the config here.
  void pick(ConfigService& config, std::string_view preset) {
    const desktop_layout::Snapshot now = desktop_layout::snapshot(config.config(), nullptr);
    std::vector<desktop_layout::Write> writes;
    for (auto& write : desktop_layout::writes(preset, now)) {
      if (write.first.front() != "andromeda") {
        writes.push_back(std::move(write));
      }
    }
    std::vector<desktop_layout::Path> clears;
    for (auto& path : desktop_layout::clears(preset, now)) {
      if (path.front() != "andromeda") {
        clears.push_back(std::move(path));
      }
    }
    bool changed = false;
    const auto stash = desktop_layout::stash(preset, now);
    expect(
        config.mutateOverrides(writes, clears, &changed, stash ? &*stash : nullptr), std::format("{} commit", preset)
    );
  }

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("andromeda-desktop-layout-switch-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  writeFile(root / "config" / "andromeda" / "shell" / "config.toml", "[bar.default]\n");
  ::setenv("ANDROMEDA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);

  // The user's own bar, set in Settings before any layout pick.
  const std::vector<desktop_layout::Write> own{
      {{"bar", "default", "end"}, std::vector<std::string>{"tray", "battery", "notifications"}},
      {{"bar", "default", "widget_spacing"}, std::int64_t{13}},
      {{"bar", "default", "margin_ends"}, std::int64_t{12}},
  };

  std::map<std::string, std::string> expected;
  {
    for (const std::string_view preset : desktop_layout::kPresets) {
      std::filesystem::remove_all(root / "state");
      ConfigService config;
      expect(config.setOverrides(own), "own bar writes");
      pick(config, preset);
      expected[std::string(preset)] = barState(config.config());
    }
  }

  const auto& presets = desktop_layout::kPresets;
  for (const std::string_view a : presets) {
    for (const std::string_view b : presets) {
      for (const std::string_view c : presets) {
        std::filesystem::remove_all(root / "state");
        ConfigService config;
        expect(config.setOverrides(own), "own bar writes");
        std::string history;
        for (const std::string_view preset : {a, b, c}) {
          pick(config, preset);
          history += std::string(history.empty() ? "" : " > ") + std::string(preset);
          const std::string got = barState(config.config());
          expect(
              got == expected[std::string(preset)],
              std::format("after {}\n  got  {}\n  want {}", history, got, expected[std::string(preset)])
          );
        }
      }
    }
  }

  std::filesystem::remove_all(root);
  if (g_failures == 0) {
    std::println("desktop_layout_switch: ok");
  }
  return g_failures == 0 ? 0 : 1;
}
