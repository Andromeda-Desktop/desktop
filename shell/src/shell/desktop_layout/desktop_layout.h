#pragma once

#include "config/config_types.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class ConfigService;
class Node;
class SettingsControl;

// Desktop Layouts (DESIGN.md "Desktop Layouts"): one pick sets the bar, dock, window arrangement, overview style and
// title bars together. The current layout is read back from those parts, so a later change to any part shows Custom.
namespace desktop_layout {

  inline constexpr std::array<std::string_view, 4> kPresets = {"andromeda", "macos", "windows", "tiling"};
  inline constexpr std::string_view kCustom = "custom";

  // A preset card on a 64 x 40 grid: the wallpaper with glass bar, dock or taskbar shapes and light windows (Settings
  // › Appearance gallery and the setup wizard draw the same card), `width` x `height` logical px at `scale`.
  [[nodiscard]] std::unique_ptr<Node> preview(std::string_view preset, float width, float height, float scale);

  // Settings paths: shell keys as in the config, compositor keys under the "andromeda" root.
  using Write = std::pair<std::vector<std::string>, ConfigOverrideValue>;
  using Path = std::vector<std::string>;

  // What a pick needs to know about the current parts, taken when the picker is built.
  struct Snapshot {
    std::vector<BarConfig> bars;
    std::vector<std::string> workspaceModes; // compositor "workspace.<N>.layout.mode" keys that hold a value
    bool dock = false;
  };
  // The parts the "custom" card draws (board section 6: after a later change it shows the current mix).
  void setCustomMix(const Snapshot& now);
  // The custom card's footnote: its parts joined by " + " ("Windows taskbar + dock").
  [[nodiscard]] std::string customMixLabel();
  [[nodiscard]] Snapshot snapshot(const Config& cfg, const SettingsControl* compositor);

  // The values that apply `preset`; empty for an unknown name. The first bar moves edge and, for Windows, becomes
  // the taskbar row.
  [[nodiscard]] std::vector<Write> writes(std::string_view preset, const Snapshot& now);
  // The overrides a pick takes away so the defaults return: the taskbar row when leaving Windows or macOS, the
  // radius 8 earlier builds wrote for Windows, and per-workspace arrangements. The user's own values stay.
  [[nodiscard]] std::vector<Path> clears(std::string_view preset, const Snapshot& now);
  // The user's own bar across a row: saved when a pick enters the Windows or macOS row from another bar, put back
  // when a pick leaves the row for one; nullopt otherwise.
  [[nodiscard]] std::optional<OverrideStash> stash(std::string_view preset, const Snapshot& now);
  // Applies `preset` in one config commit (writes, clears and the bar stash), then writes its compositor keys. False
  // for an unknown preset or a refused commit.
  bool apply(std::string_view preset, ConfigService& config, SettingsControl* compositor);
  // The preset whose parts all match, kCustom when none does, or empty while the compositor's settings are not known.
  [[nodiscard]] std::string current(const Config& cfg, const SettingsControl* compositor);

} // namespace desktop_layout
