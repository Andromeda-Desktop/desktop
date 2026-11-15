// Every settings row built from the config schema shows what the Settings window writes to its path,
// and shows its old value again after the override is cleared. The test writes through ConfigService,
// as the window does, and reads each row back from a rebuilt registry.
#include "config/config_service.h"
#include "config/config_types.h"
#include "core/input/key_chord.h"
#include "shell/settings/settings_registry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <print>
#include <string>
#include <unistd.h>
#include <variant>
#include <vector>

namespace {

  using namespace settings;

  std::vector<SettingEntry> registryOf(const Config& cfg) {
    RegistryEnvironment env;
    env.availableOutputs = {SelectOption{.value = "DP-1", .label = "DP-1"}};
    return buildSettingsRegistry(cfg, cfg.bars.empty() ? nullptr : &cfg.bars.front(), nullptr, env);
  }

  // The value a row shows, as text; nullopt for a control this test does not drive.
  std::optional<std::string> shownValue(const SettingControl& control) {
    if (const auto* c = std::get_if<ToggleSetting>(&control)) {
      return c->checked ? "true" : "false";
    }
    if (const auto* c = std::get_if<SelectSetting>(&control)) {
      return c->selectedValue;
    }
    if (const auto* c = std::get_if<SliderSetting>(&control)) {
      return std::format("{:.4f}", c->value);
    }
    if (const auto* c = std::get_if<StepperSetting>(&control)) {
      return std::to_string(c->value);
    }
    if (const auto* c = std::get_if<TextSetting>(&control)) {
      return c->value;
    }
    if (const auto* c = std::get_if<ColorSpecPickerSetting>(&control)) {
      return c->selectedValue;
    }
    if (const auto* c = std::get_if<OptionalStepperSetting>(&control)) {
      return c->value ? std::to_string(*c->value) : "unset";
    }
    if (const auto* c = std::get_if<ListSetting>(&control)) {
      std::string out;
      for (const auto& item : c->items) {
        out += item + ";";
      }
      return out;
    }
    if (const auto* c = std::get_if<KeybindListSetting>(&control)) {
      std::string out;
      for (const auto& chord : c->items) {
        out += keyChordToString(chord) + ";";
      }
      return out;
    }
    return std::nullopt;
  }

  struct Write {
    ConfigOverrideValue value;
    std::string shown;
  };

  // A value that differs from the shown one, as the control would commit it.
  std::optional<Write> nextValue(const SettingControl& control) {
    if (const auto* c = std::get_if<ToggleSetting>(&control)) {
      if (!c->enabled) {
        return std::nullopt;
      }
      return Write{!c->checked, c->checked ? "false" : "true"};
    }
    if (const auto* c = std::get_if<SelectSetting>(&control)) {
      const auto it = std::ranges::find_if(c->options, [&](const SelectOption& o) {
        return !o.value.empty() && o.value != c->selectedValue;
      });
      if (it == c->options.end()) {
        return std::nullopt;
      }
      return Write{it->value, it->value};
    }
    if (const auto* c = std::get_if<SliderSetting>(&control)) {
      const double v = c->value + c->step <= c->maxValue ? c->value + c->step : c->value - c->step;
      if (c->integerValue) {
        return Write{static_cast<std::int64_t>(std::lround(v)), std::format("{:.4f}", std::round(v))};
      }
      return Write{v, std::format("{:.4f}", v)};
    }
    if (const auto* c = std::get_if<StepperSetting>(&control)) {
      const int v = c->value + c->step <= c->maxValue ? c->value + c->step : c->value - c->step;
      return Write{static_cast<std::int64_t>(v), std::to_string(v)};
    }
    if (const auto* c = std::get_if<TextSetting>(&control)) {
      const std::string v = c->value == "roundtrip" ? "roundtrip-2" : "roundtrip";
      return Write{v, v};
    }
    if (const auto* c = std::get_if<ColorSpecPickerSetting>(&control)) {
      const std::string v = c->selectedValue == "#123456" ? "#654321" : "#123456";
      return Write{v, v};
    }
    if (const auto* c = std::get_if<OptionalStepperSetting>(&control)) {
      const int v = c->value.value_or(c->fallbackValue) + 1 <= c->maxValue ? c->value.value_or(c->fallbackValue) + 1
                                                                           : c->minValue;
      return Write{static_cast<std::int64_t>(v), std::to_string(v)};
    }
    if (std::holds_alternative<ListSetting>(control)) {
      return Write{std::vector<std::string>{"roundtrip"}, "roundtrip;"};
    }
    if (std::holds_alternative<KeybindListSetting>(control)) {
      const KeyChord chord = *parseKeyChordSpec("Ctrl+F12");
      return Write{std::vector<KeyChord>{chord}, keyChordToString(chord) + ";"};
    }
    return std::nullopt;
  }

  std::string joined(const std::vector<std::string>& path) {
    std::string out;
    for (const auto& part : path) {
      out += (out.empty() ? "" : ".") + part;
    }
    return out;
  }

  const SettingEntry* rowAt(const std::vector<SettingEntry>& entries, const std::vector<std::string>& path) {
    const auto it = std::ranges::find(entries, path, &SettingEntry::path);
    return it == entries.end() ? nullptr : &*it;
  }

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("settings-rows-roundtrip-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "config" / "andromeda" / "shell");
  std::ofstream(root / "config" / "andromeda" / "shell" / "config.toml") << "[bar.default]\nenabled = true\n"
                                                                      // custom_schedule needs both times to validate.
                                                                      "[location]\nsunrise = \"06:00\"\nsunset = \"20:00\"\n";
  ::setenv("ANDROMEDA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);
  ::setenv("HOME", root.c_str(), 1);

  ConfigService config;
  std::vector<std::vector<std::string>> paths;
  for (const auto& entry : registryOf(config.config())) {
    if (entry.fromSchema) {
      paths.push_back(entry.path);
    }
  }

  bool ok = !paths.empty();
  std::size_t checked = 0;
  for (const auto& path : paths) {
    const auto before = registryOf(config.config());
    const SettingEntry* row = rowAt(before, path);
    if (row == nullptr) {
      continue;
    }
    const auto oldShown = shownValue(row->control);
    const auto write = nextValue(row->control);
    if (!oldShown || !write) {
      continue;
    }
    if (!config.setOverride(path, write->value)) {
      ok = false;
      std::println(stderr, "{}: override rejected", joined(path));
      continue;
    }
    const auto after = registryOf(config.config());
    const SettingEntry* written = rowAt(after, path);
    const auto shown = written != nullptr ? shownValue(written->control) : std::nullopt;
    if (shown != write->shown) {
      ok = false;
      std::println(stderr, "{}: wrote {}, row shows {}", joined(path), write->shown, shown.value_or("<no row>"));
    }
    config.clearOverrides({path}, nullptr);
    const auto reset = registryOf(config.config());
    const SettingEntry* cleared = rowAt(reset, path);
    const auto resetShown = cleared != nullptr ? shownValue(cleared->control) : std::nullopt;
    if (resetShown != oldShown) {
      ok = false;
      std::println(stderr, "{}: was {}, after reset shows {}", joined(path), *oldShown, resetShown.value_or("<no row>"));
    }
    ++checked;
  }
  std::println("checked {} of {} schema rows", checked, paths.size());
  std::filesystem::remove_all(root);
  return ok && checked > 0 ? 0 : 1;
}
