#pragma once

#include "shell/settings/settings_registry.h"
#include "config/app_icon_settings.h"

#include "config/config_types.h"
#include "config/schema/config_schema.h"
#include "config/schema/ranges.h"
#include "core/files/directory_scanner.h"
#include "core/log.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "launcher/panel_catalog.h"
#include "pipewire/sound_player.h"
#include "shell/bar/widget_gesture.h"
#include "shell/bar/widget_gesture_defaults.h"
#include "shell/control_center/control_center_panel.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/settings/color_spec_picker.h"
#include "shell/settings/font_weight_catalog.h"
#include "shell/wallpaper/wallpaper_paths.h"
#include "system/sysmon_threshold_profile.h"
#include "ui/app_icon_colorization.h"
#include "util/file_utils.h"
#include "util/string_utils.h"
#include "compositors/settings_control.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace settings {
  namespace registry_detail {

    // Launcher providers that expose a configurable prefix, keyed by the config name
    // (the lowercased provider id). Placeholders and global-search defaults mirror the
    // provider's own defaultPrefix()/defaultIncludeInGlobalSearch().
    struct LauncherProviderSettingSpec {
      std::string_view name;
      std::string_view prefixPlaceholder;
      bool globalByDefault = false;
    };

    constexpr auto kLauncherProviderSettings = std::to_array<LauncherProviderSettingSpec>({
        {.name = "calculator", .prefixPlaceholder = "calc", .globalByDefault = true},
        {.name = "emoji", .prefixPlaceholder = "emo"},
        {.name = "panels", .prefixPlaceholder = "pan"},
        {.name = "session", .prefixPlaceholder = "session"},
        {.name = "wallpaper", .prefixPlaceholder = "wall"},
        {.name = "windows", .prefixPlaceholder = "win"},
    });

    [[nodiscard]] SliderSetting barCornerSlider(std::int32_t value);
    [[nodiscard]] std::string deadZoneDefault(noctalia::bar::Gesture gesture);
    [[nodiscard]] SliderSetting barReservedSlider(double value, double maxValue, double step, bool integer);
    SelectSetting asSegmented(SelectSetting setting);
    StepperSetting barMarginStepper(std::int32_t value);
    StepperSetting barPanelOverlapStepper(std::int32_t value);
    SelectSetting
    plainSelect(std::initializer_list<std::pair<std::string_view, std::string_view>> items, std::string_view selected);
    [[nodiscard]] std::string_view barAutoHideMode(bool autoHide, bool smartAutoHide);
    [[nodiscard]] SelectSetting autoHideModeSelect(std::string_view mode, std::vector<std::string> smartPath);
    std::vector<SelectOption> controlCenterShortcutOptions(const Config& cfg);
    [[nodiscard]] inline bool
    hasFeature(const RegistryEnvironment& env, noctalia::config::schema::RowSpec::Needs needs) {
      using Needs = noctalia::config::schema::RowSpec::Needs;
      switch (needs) {
      case Needs::None:
        return true;
      case Needs::Ddcutil:
        return env.ddcutilAvailable;
      case Needs::SystemdUser:
        return env.systemdUserManaged;
      case Needs::GammaControl:
        return env.gammaControlAvailable;
      }
      return true;
    }
    // Choices known only at runtime: select options or list suggestions. Outputs, Fonts and BatteryDevices need `env`.
    std::vector<SelectOption>
    runtimeOptions(noctalia::config::schema::RowSpec::Options options, const RegistryEnvironment* env);
    ColorSpecPickerSetting
    colorSpecPicker(const std::optional<ColorSpec>& selected, bool allowNone, std::string noneLabel = {});
    ColorSpecPickerSetting
    colorSpecPicker(const ColorSpec& selected, bool allowNone = false, std::string noneLabel = {});
    std::string pathText(const std::vector<std::string>& path);
    SettingEntry makeEntry(
        SettingsSection section, std::string group, std::string title, std::string subtitle,
        std::vector<std::string> path, SettingControl control, const std::string& tags = {}, bool advanced = false
    );
    void appendGroup(std::vector<SettingEntry>& entries, std::vector<SettingEntry>& generated, std::string_view group);
    void appendRow(std::vector<SettingEntry>& entries, std::vector<SettingEntry>& generated, std::string_view key);

    // Rows for the fields of `schema` that have a RowSpec, in schema order. The current value is read back
    // through the field's own write(), so a row shows exactly what the config file would hold.
    template <typename Struct>
    std::vector<SettingEntry> generateSchemaEntries(
        const noctalia::config::schema::Schema<Struct>& schema, const Struct& values,
        const std::vector<std::string>& prefix, const RegistryEnvironment* env = nullptr
    ) {
      using noctalia::config::schema::RowSpec;
      std::vector<SettingEntry> entries;
      for (const auto& field : schema) {
        if (!field.row) {
          continue;
        }
        const RowSpec& row = *field.row;
        assert(env || row.needs == RowSpec::Needs::None);
        assert(row.needs == RowSpec::Needs::None || (row.kind == RowSpec::Kind::Toggle && !row.missingKey.empty()));
        toml::table written;
        field.write(written, values);
        const auto value = std::as_const(written)[field.key];
        SettingControl control;
        switch (row.kind) {
        case RowSpec::Kind::Toggle:
          control = ToggleSetting{value.value_or(false)};
          break;
        case RowSpec::Kind::Slider: {
          SliderSetting slider{
              noctalia::config::schema::finiteDouble(value).value_or(0.0), row.range.min.value(),
              row.range.max.value(), row.range.step.value(), row.integer
          };
          slider.valueSuffix = row.suffix;
          control = std::move(slider);
          break;
        }
        case RowSpec::Kind::Select: {
          SelectSetting select;
          select.options = runtimeOptions(row.options, env);
          for (const auto& choice : row.choices) {
            select.options.push_back(SelectOption{std::string(choice.value), i18n::tr(choice.labelKey)});
          }
          select.clearOnEmpty = row.clearOnEmpty;
          select.selectedValue = value.value_or(std::string{});
          select.segmented = row.segmented;
          select.preferredWidth = row.width;
          control = std::move(select);
          break;
        }
        case RowSpec::Kind::Text:
          control = TextSetting{
              .value = value.value_or(std::string{}),
              .placeholder = row.placeholderKey.empty() ? std::string(row.placeholder) : i18n::tr(row.placeholderKey),
              .width = row.width,
              .browseMode = row.browse == RowSpec::Browse::Folder ? TextSettingBrowseMode::SelectFolder
                  : row.browse == RowSpec::Browse::File           ? TextSettingBrowseMode::OpenFile
                                                                  : TextSettingBrowseMode::None,
          };
          break;
        case RowSpec::Kind::Stepper:
          control = StepperSetting{
              .value = std::clamp(
                  static_cast<int>(std::lround(noctalia::config::schema::finiteDouble(value).value_or(0.0))),
                  static_cast<int>(row.range.min.value()), static_cast<int>(row.range.max.value())
              ),
              .minValue = static_cast<int>(row.range.min.value()),
              .maxValue = static_cast<int>(row.range.max.value()),
              .step = static_cast<int>(row.range.step.value_or(1.0)),
              .valueSuffix = std::string(row.suffix),
          };
          break;
        case RowSpec::Kind::Color:
          control = ColorSpecPickerSetting{
              .roles = {},
              .selectedValue = value.value_or(std::string{}),
              .allowNone = row.allowNone,
              .allowCustomColor = true,
              .noneLabel = row.unsetLabelKey.empty() ? std::string{} : i18n::tr(row.unsetLabelKey),
          };
          break;
        case RowSpec::Kind::OptionalStepper: {
          const int minValue = static_cast<int>(row.range.min.value());
          const int maxValue = static_cast<int>(row.range.max.value());
          std::optional<int> current;
          if (const auto v = noctalia::config::schema::finiteDouble(value)) {
            current = std::clamp(static_cast<int>(std::lround(*v)), minValue, maxValue);
          }
          control = OptionalStepperSetting{
              .value = current,
              .minValue = minValue,
              .maxValue = maxValue,
              .step = static_cast<int>(row.range.step.value_or(1.0)),
              .fallbackValue = current.value_or(row.fallback),
              .unsetLabel = i18n::tr(row.unsetLabelKey),
              .customLabel = i18n::tr("common.states.custom"),
          };
          break;
        }
        case RowSpec::Kind::List: {
          ListSetting list;
          if (const auto* items = value.as_array()) {
            for (const auto& item : *items) {
              if (const auto text = item.template value<std::string>()) {
                list.items.push_back(*text);
              }
            }
          }
          list.suggestedOptions = runtimeOptions(row.options, env);
          control = std::move(list);
          break;
        }
        case RowSpec::Kind::KeybindList: {
          KeybindListSetting keybinds{.items = {}, .maxItems = row.maxItems};
          if (const auto* items = value.as_array()) {
            for (const auto& item : *items) {
              if (const auto text = item.template value<std::string>()) {
                if (auto chord = parseKeyChordSpec(*text)) {
                  keybinds.items.push_back(*chord);
                }
              }
            }
          }
          control = std::move(keybinds);
          break;
        }
        }
        std::vector<std::string> path = prefix;
        path.emplace_back(field.key);
        const std::string label = row.label.empty() ? std::string(row.section) + "." + i18n::keySegment(field.key)
                                                    : std::string(row.label);
        const bool featureMissing = row.needs != RowSpec::Needs::None && !hasFeature(*env, row.needs);
        if (featureMissing) {
          auto& toggle = std::get<ToggleSetting>(control);
          toggle.enabled = toggle.checked;
        }
        entries.push_back(makeEntry(
            settingsSectionFromId(row.section).value(), std::string(row.group),
            i18n::tr("settings.schema." + (row.title.empty() ? label : std::string(row.title)) + ".label"),
            i18n::tr("settings.schema." + label + "." + (featureMissing ? std::string(row.missingKey) : "description")),
            std::move(path), std::move(control), std::string(row.tags), row.advanced
        ));
        entries.back().fromSchema = true;
      }
      return entries;
    }

    // Rows of an override table whose colour and optional-stepper keys may stay unset. Each takes the RowSpec
    // of the parent field with the same key; unset shows "inherit", and the stepper leaves it at the parent value.
    template <typename Parent, typename Override>
    std::vector<SettingEntry> generateInheritEntries(
        const noctalia::config::schema::Schema<Parent>& parentSchema, const Parent& parent,
        const noctalia::config::schema::Schema<Override>& overrideSchema, const Override& values,
        const std::vector<std::string>& prefix
    ) {
      using noctalia::config::schema::RowSpec;
      noctalia::config::schema::Schema<Override> schema;
      for (const auto& parentField : parentSchema) {
        if (!parentField.row
            || (parentField.row->kind != RowSpec::Kind::Color && parentField.row->kind != RowSpec::Kind::OptionalStepper)) {
          continue;
        }
        const auto it = std::ranges::find(overrideSchema, parentField.key, &noctalia::config::schema::Field<Override>::key);
        if (it == overrideSchema.end()) {
          continue;
        }
        RowSpec row = *parentField.row;
        row.allowNone = true;
        row.unsetLabelKey = "common.states.inherit";
        if (row.kind == RowSpec::Kind::OptionalStepper) {
          toml::table written;
          parentField.write(written, parent);
          if (const auto v = noctalia::config::schema::finiteDouble(std::as_const(written)[parentField.key])) {
            row.fallback = std::clamp(
                static_cast<int>(std::lround(*v)), static_cast<int>(row.range.min.value()),
                static_cast<int>(row.range.max.value())
            );
          }
        }
        auto field = *it;
        field.row = std::move(row);
        schema.push_back(std::move(field));
      }
      return generateSchemaEntries(schema, values, prefix);
    }


    struct RegistryContext {
      const Config& cfg;
      const RegistryEnvironment& env;
      std::vector<SettingEntry> entries;
      std::function<SelectSetting(std::string_view)> positionSelect;
      std::vector<SettingEntry> shellRows;
      std::vector<SettingEntry> panelRows;
    };

    void addCompositorEntries(
        std::vector<SettingEntry>& entries, const SettingsControl& compositor,
        const std::function<void(std::string)>& openDetail
    );
    void add_appearance_settings(RegistryContext& ctx);
    void add_wallpaper_settings(RegistryContext& ctx);
    void add_dock_settings(RegistryContext& ctx);
    void add_panels_settings(RegistryContext& ctx);
    void add_controlcenter_settings(RegistryContext& ctx);
    void add_desktop_settings(RegistryContext& ctx);
    void add_security_settings(RegistryContext& ctx);
    void add_shell_settings(RegistryContext& ctx);
    void add_system_settings(RegistryContext& ctx);
    void add_power_settings(RegistryContext& ctx);
    void add_notifications_settings(RegistryContext& ctx);
    void add_osd_settings(RegistryContext& ctx);
    void add_bar_settings(RegistryContext& ctx);
    void add_baroverrides_settings(RegistryContext& ctx);

  } // namespace registry_detail
} // namespace settings
