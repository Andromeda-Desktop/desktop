// Desktop Layout picks replayed as Settings commits them: a snapshot of the current config, then the pick's writes
// and clears in one commit (SettingsWindow::mutateSettingOverrides). Every sequence of up to three picks must leave
// the bar a pure function of the last pick, so a pick applies in one tap and leaving the Windows or macOS row gives
// the user's own bar back.

#include "async_file_wait.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "shell/desktop_layout/desktop_layout.h"
#include "util/file_utils.h"

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

  // A fresh state dir: the last instance's queued override and state writes land first, or they recreate it.
  void resetState(const std::filesystem::path& root) {
    const std::filesystem::path state = FileUtils::stateDir();
    expect(waitFileWritten(state / "overrides.toml") && waitFileWritten(state / "state.toml"), "state writes landed");
    std::filesystem::remove_all(root / "state");
  }

  std::string join(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
      out += name + ",";
    }
    return out;
  }

  std::string widgetSetting(const Config& cfg, const std::string& widget, const std::string& key) {
    const auto it = cfg.widgets.find(widget);
    if (it == cfg.widgets.end()) {
      return "-";
    }
    const auto value = it->second.settings.find(key);
    if (value == it->second.settings.end()) {
      return "-";
    }
    if (const auto* b = std::get_if<bool>(&value->second)) {
      return *b ? "true" : "false";
    }
    if (const auto* text = std::get_if<std::string>(&value->second)) {
      return *text;
    }
    if (const auto* count = std::get_if<std::int64_t>(&value->second)) {
      return std::to_string(*count);
    }
    return "?";
  }

  std::string barState(const Config& cfg) {
    const BarConfig& bar = cfg.bars.front();
    return std::format(
        "islands={} position={} dock={} thickness={} margin_ends={} margin_edge={} padding={} padding_end={} "
        "spacing={} start=[{}] center=[{}] end=[{}] background={} blur={} shadow={} border={} solid={} groups={} "
        "battery_label={} launcher_glyph={} tray_slots={}",
        bar.islands, bar.position, cfg.dock.enabled, bar.thickness, bar.marginEnds, bar.marginEdge, bar.padding,
        bar.paddingEnd, bar.widgetSpacing, join(bar.startWidgets), join(bar.centerWidgets), join(bar.endWidgets),
        bar.backgroundOpacity, bar.compositorBlur, bar.shadow, bar.borderWidth,
        bar.solidOpacity ? std::format("{}", *bar.solidOpacity) : std::string("-"), bar.widgetCapsuleGroups.size(),
        widgetSetting(cfg, "status_battery", "show_label"), widgetSetting(cfg, "launcher_button", "glyph"),
        widgetSetting(cfg, "tray", "bar_slots")
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
      resetState(root);
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
        resetState(root);
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

  // A Windows bar saved before the row gained task_view is still the Windows layout, and picks from it stay pure.
  for (const std::string_view preset : presets) {
    resetState(root);
    ConfigService config;
    expect(config.setOverrides(own), "own bar writes");
    pick(config, "windows");
    expect(
        config.setOverrides({{{"bar", "default", "center"}, std::vector<std::string>{"start", "windows_taskbar"}}}),
        "old windows row"
    );
    pick(config, preset);
    const std::string got = barState(config.config());
    expect(
        got == expected[std::string(preset)],
        std::format("old windows > {}\n  got  {}\n  want {}", preset, got, expected[std::string(preset)])
    );
  }

  // Bars saved with the v1 rows still read as their preset: every pick from them clears the row like from v2.
  const std::vector<std::pair<std::string_view, desktop_layout::Write>> v1Rows{
      {"windows", {{"bar", "default", "start"}, std::vector<std::string>{}}},
      {"windows", {{"bar", "default", "center"}, std::vector<std::string>{"start", "task_view", "windows_taskbar"}}},
      {"windows",
       {{"bar", "default", "end"},
        std::vector<std::string>{"tray", "group:status", "windows_clock", "notifications", "show_desktop"}}},
      {"macos", {{"bar", "default", "end"}, std::vector<std::string>{"group:status", "macos_clock", "notifications"}}},
  };
  for (const std::string_view from : {"windows", "macos"}) {
    for (const std::string_view preset : presets) {
      resetState(root);
      ConfigService config;
      expect(config.setOverrides(own), "own bar writes");
      pick(config, from);
      std::vector<desktop_layout::Write> v1;
      for (const auto& [name, write] : v1Rows) {
        if (name == from) {
          v1.push_back(write);
        }
      }
      expect(config.setOverrides(v1), std::format("v1 {} row", from));
      pick(config, preset);
      const std::string got = barState(config.config());
      expect(
          got == expected[std::string(preset)],
          std::format("v1 {} > {}\n  got  {}\n  want {}", from, preset, got, expected[std::string(preset)])
      );
    }
  }

  std::filesystem::remove_all(root);
  if (g_failures == 0) {
    std::println("desktop_layout_switch: ok");
  }
  return g_failures == 0 ? 0 : 1;
}
