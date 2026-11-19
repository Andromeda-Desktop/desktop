#include "shell/settings/settings_registry_internal.h"
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
#include "theme/builtin_palettes.h"
#include "ui/app_icon_colorization.h"
#include "util/file_utils.h"
#include "util/string_utils.h"
#include "compositors/settings_control.h"

#include <algorithm>
#include <array>
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
  using namespace registry_detail;

  namespace {
    std::string deadZoneGroup(noctalia::bar::Gesture gesture) {
      return noctalia::bar::scrollDirectionForGesture(gesture).has_value() ? "empty-scroll" : "empty-clicks";
    }
  } // namespace

  void registry_detail::add_osd_settings(RegistryContext& ctx) {
    using i18n::tr;
    const auto& cfg = ctx.cfg;
    const auto& env = ctx.env;
    auto& entries = ctx.entries;

    auto osdRows = generateSchemaEntries(noctalia::config::schema::osdSchema(), cfg.osd, {"osd"}, &env);
    appendRow(entries, osdRows, "enabled");
    const std::size_t osdGatedStart = entries.size();
    for (const std::string_view key :
         {"orientation", "position", "position_vertical", "hide_delay_ms", "scale", "offset_x", "offset_y",
          "background_opacity", "border", "border_color", "border_width", "follow_focused_output", "monitors"}) {
      appendRow(entries, osdRows, key);
    }
    entries.back().visibleWhen = [](const Config& c) { return !c.osd.followFocusedOutput; };
    auto kindRows = generateSchemaEntries(noctalia::config::schema::osdKindsSchema(), cfg.osd.kinds, {"osd", "kinds"});
    for (auto& row : kindRows) {
      const std::string& key = row.path.back();
      if (key == "volume_output" || key == "volume_input") {
        row.visibleWhen = [](const Config& c) { return c.osd.kinds.volume; };
      } else if (key == "media_volume") {
        row.visibleWhen = [](const Config& c) { return c.osd.kinds.media; };
      }
      entries.push_back(std::move(row));
    }
    // Gate every OSD entry after the master toggle on osd.enabled, preserving any per-entry visibility.
    for (std::size_t i = osdGatedStart; i < entries.size(); ++i) {
      SettingVisibility prev = std::move(entries[i].visibleWhen);
      entries[i].visibleWhen = prev
          ? SettingVisibility{[prev = std::move(prev)](const Config& c) { return c.osd.enabled && prev(c); }}
          : SettingVisibility{[](const Config& c) { return c.osd.enabled; }};
    }
  }

  void registry_detail::add_bar_settings(RegistryContext& ctx) {
    using i18n::tr;
    const auto& cfg = ctx.cfg;
    const auto& env = ctx.env;
    auto& entries = ctx.entries;
    const auto& positionSelect = ctx.positionSelect;

    // Bar: register every configured bar so global search can surface settings from all of them.
    for (const auto& bar : cfg.bars) {
      constexpr SettingsSection section = SettingsSection::Bar;
      const std::vector<std::string> root = {"bar", bar.name};
      auto path = [&](std::string key) {
        std::vector<std::string> p = root;
        p.push_back(std::move(key));
        return p;
      };
      auto barRows = generateSchemaEntries(noctalia::config::schema::barFieldsSchema(), bar, root);
      appendRow(entries, barRows, "enabled");
      entries.push_back(makeEntry(
          section, "general", tr("settings.schema.shared.position.label"),
          tr("settings.schema.bar.position.description"), path("position"), positionSelect(bar.position), "edge"
      ));
      entries.push_back(makeEntry(
          section, "general", tr("settings.schema.shared.auto-hide.label"),
          tr("settings.schema.bar.auto-hide.description"), path("auto_hide"),
          autoHideModeSelect(barAutoHideMode(bar.autoHide, bar.smartAutoHide), path("smart_auto_hide")),
          "autohide smart workspace"
      ));
      appendRow(entries, barRows, "auto_hide_delay_ms");
      entries.back().visibleWhen = [barName = bar.name](const Config& c) {
        const BarConfig* b = findBar(c, barName);
        return b != nullptr && b->autoHide && !b->smartAutoHide;
      };
      const SettingVisibility autoHideOn = [barName = bar.name](const Config& c) {
        const BarConfig* b = findBar(c, barName);
        return b != nullptr && b->isAutoHideEnabled();
      };
      appendRow(entries, barRows, "show_on_workspace_switch");
      entries.back().visibleWhen = autoHideOn;
      for (const auto* key : {"reserve_space", "layer", "thickness", "scale", "font_scale", "margin_ends", "margin_edge",
                              "margin_opposite_edge", "padding", "radius", "radius_top_left", "radius_top_right",
                              "radius_bottom_left", "radius_bottom_right"}) {
        appendRow(entries, barRows, key);
      }
      appendRow(entries, barRows, "concave_edge_corners");
      entries.back().visibleWhen = [barName = bar.name](const Config& c) {
        const BarConfig* b = findBar(c, barName);
        return b != nullptr && b->marginEdge == 0;
      };
      for (const auto* key : {"border", "border_width", "islands", "background_opacity"}) {
        appendRow(entries, barRows, key);
      }
      if (env.backgroundEffectBlurSupported) {
        appendRow(entries, barRows, "compositor_blur");
      }
      for (const auto* key : {"shadow", "contact_shadow", "panel_overlap"}) {
        appendRow(entries, barRows, key);
      }
      const std::string barResolvedFontFamily =
          bar.fontFamily && !bar.fontFamily->empty() ? *bar.fontFamily : cfg.shell.fontFamily;
      {
        SettingControl fontFamilyControl = TextSetting{
            .value = bar.fontFamily.value_or(""), .placeholder = cfg.shell.fontFamily, .browseFileExtensions = {}
        };
        if (!env.fontFamilies.empty()) {
          fontFamilyControl = SearchPickerSetting{
              .options = env.fontFamilies,
              .selectedValue = bar.fontFamily.value_or(""),
              .placeholder = cfg.shell.fontFamily,
              .emptyText = tr("ui.controls.search-picker.empty"),
              .preferredHeight = 280.0F,
          };
        }
        entries.push_back(makeEntry(
            section, "widgets", tr("settings.schema.bar.font-family.label"),
            tr("settings.schema.bar.font-family.description"), path("font_family"), std::move(fontFamilyControl),
            "typeface font"
        ));
      }
      {
        std::vector<SelectOption> fontWeightOptions;
        const auto widgetOptions =
            buildLabelFontWeightSelectOptions(barResolvedFontFamily, FontWeightSelectKind::BarDefault, bar.fontWeight);
        fontWeightOptions.reserve(widgetOptions.size());
        for (const auto& option : widgetOptions) {
          fontWeightOptions.push_back(SelectOption{option.value, tr(option.labelKey)});
        }
        SelectSetting fontWeightSelect{std::move(fontWeightOptions), std::to_string(bar.fontWeight)};
        fontWeightSelect.valueType = SelectValueType::Integer;
        entries.push_back(makeEntry(
            section, "widgets", tr("settings.schema.bar.font-weight.label"),
            tr("settings.schema.bar.font-weight.description"), path("font_weight"), std::move(fontWeightSelect),
            "font text weight"
        ));
      }
      for (const auto* key : {"widget_spacing", "color", "icon_color", "hover_highlight", "show_tooltip", "capsule", "capsule_thickness"}) {
        appendRow(entries, barRows, key);
      }
      const SettingVisibility capsuleOn = [on = bar.widgetCapsuleDefault](const Config&) { return on; };
      appendRow(entries, barRows, "capsule_radius");
      for (const auto* key : {"capsule_fill", "capsule_foreground"}) {
        appendRow(entries, barRows, key);
        entries.back().visibleWhen = capsuleOn;
      }
      for (const auto* key : {"capsule_border", "capsule_border_width", "capsule_padding", "capsule_opacity"}) {
        appendRow(entries, barRows, key);
        entries.back().visibleWhen = capsuleOn;
      }
      for (const auto* key : {"start", "center", "end"}) {
        appendRow(entries, barRows, key);
      }
      auto deadZoneRows = generateSchemaEntries(
          noctalia::config::schema::barDeadZoneSchema(), bar.deadZone, std::vector<std::string>{"bar", bar.name, "dead_zone"}
      );
      appendRow(entries, deadZoneRows, "click_through");
      entries.back().visibleWhen = [autoHideOn](const Config& c) { return !autoHideOn(c); };
      // One row per gesture, from the same closed set widget actions use. An unset row shows the
      // built-in default as its placeholder, so "empty" never reads as "does nothing".
      for (const auto gesture : noctalia::bar::allGestures()) {
        const std::string key(noctalia::bar::gestureConfigKey(gesture));
        const auto configured = bar.deadZone.actions.find(key);
        entries.push_back(makeEntry(
            section, deadZoneGroup(gesture), tr(noctalia::bar::gestureLabelKey(gesture)),
            tr("settings.schema.bar.dead-zone-action.description"),
            std::vector<std::string>{"bar", bar.name, "dead_zone", "actions", key},
            GestureActionSetting{
                .gestureKey = key,
                .configured = configured != bar.deadZone.actions.end() ? configured->second : std::string{},
                .defaultAction = deadZoneDefault(gesture),
            },
            "bar empty margin dead zone action command gesture " + key
        ));
      }
    }
  }

  void registry_detail::add_baroverrides_settings(RegistryContext& ctx) {
    using i18n::tr;
    const auto& cfg = ctx.cfg;
    const auto& env = ctx.env;
    auto& entries = ctx.entries;
    const auto& positionSelect = ctx.positionSelect;

    // Bar monitor overrides (all bars).
    for (const auto& bar : cfg.bars) {
      for (const auto& ovr : bar.monitorOverrides) {
        constexpr SettingsSection section = SettingsSection::Bar;
        const std::vector<std::string> root = {"bar", bar.name, "monitor", ovr.match};
        auto monitorPath = [&](std::string key) {
          std::vector<std::string> p = root;
          p.push_back(std::move(key));
          return p;
        };

        const BarConfig resolved = resolveBarMonitorOverride(bar, ovr);
        auto ovrRows = generateSchemaEntries(noctalia::config::schema::barFieldsSchema(), resolved, root);
        auto inheritRows = generateInheritEntries(
            noctalia::config::schema::barFieldsSchema(), bar, noctalia::config::schema::barMonitorOverrideSchema(), ovr,
            root
        );
        appendRow(entries, ovrRows, "enabled");
        entries.push_back(makeEntry(
            section, "general", tr("settings.schema.shared.position.label"),
            tr("settings.schema.bar.position.description"), monitorPath("position"),
            positionSelect(ovr.position.value_or(bar.position)), "edge"
        ));
        entries.push_back(makeEntry(
            section, "general", tr("settings.schema.shared.auto-hide.label"),
            tr("settings.schema.bar.auto-hide.description"), monitorPath("auto_hide"),
            autoHideModeSelect(
                barAutoHideMode(ovr.autoHide.value_or(bar.autoHide), ovr.smartAutoHide.value_or(bar.smartAutoHide)),
                monitorPath("smart_auto_hide")
            ),
            "autohide smart workspace"
        ));
        appendRow(entries, ovrRows, "auto_hide_delay_ms");
        entries.back().visibleWhen = [barName = bar.name, match = ovr.match](const Config& c) {
          const BarConfig* b = findBar(c, barName);
          if (b == nullptr) {
            return false;
          }
          const BarMonitorOverride* o = findMonitorOverride(*b, match);
          const bool autoHide = o != nullptr ? o->autoHide.value_or(b->autoHide) : b->autoHide;
          const bool smartAutoHide = o != nullptr ? o->smartAutoHide.value_or(b->smartAutoHide) : b->smartAutoHide;
          return autoHide && !smartAutoHide;
        };
        const SettingVisibility monitorAutoHideOn = [barName = bar.name, match = ovr.match](const Config& c) {
          const BarConfig* b = findBar(c, barName);
          if (b == nullptr) {
            return false;
          }
          const BarMonitorOverride* o = findMonitorOverride(*b, match);
          return o != nullptr ? o->isAutoHideEnabled(b->autoHide, b->smartAutoHide) : b->isAutoHideEnabled();
        };
        appendRow(entries, ovrRows, "show_on_workspace_switch");
        entries.back().visibleWhen = monitorAutoHideOn;
        for (const auto* key :
             {"reserve_space", "layer", "thickness", "scale", "font_scale", "margin_ends", "margin_edge",
              "margin_opposite_edge", "padding", "radius", "radius_top_left", "radius_top_right", "radius_bottom_left",
              "radius_bottom_right"}) {
          appendRow(entries, ovrRows, key);
        }
        appendRow(entries, ovrRows, "concave_edge_corners");
        entries.back().visibleWhen = [barName = bar.name, match = ovr.match](const Config& c) {
          const BarConfig* b = findBar(c, barName);
          if (b == nullptr) {
            return false;
          }
          const BarMonitorOverride* mo = findMonitorOverride(*b, match);
          const std::int32_t marginEdge = mo && mo->marginEdge.has_value() ? *mo->marginEdge : b->marginEdge;
          return marginEdge == 0;
        };
        appendRow(entries, inheritRows, "border");
        for (const auto* key : {"border_width", "background_opacity"}) {
          appendRow(entries, ovrRows, key);
        }
        if (env.backgroundEffectBlurSupported) {
          appendRow(entries, ovrRows, "compositor_blur");
        }
        for (const auto* key : {"shadow", "contact_shadow", "panel_overlap"}) {
          appendRow(entries, ovrRows, key);
        }
        {
          const std::string monitorInheritedFontFamily = bar.fontFamily.value_or(cfg.shell.fontFamily);
          SettingControl fontFamilyControl = TextSetting{
              .value = ovr.fontFamily.value_or(""),
              .placeholder = monitorInheritedFontFamily,
              .browseFileExtensions = {}
          };
          if (!env.fontFamilies.empty()) {
            fontFamilyControl = SearchPickerSetting{
                .options = env.fontFamilies,
                .selectedValue = ovr.fontFamily.value_or(""),
                .placeholder = monitorInheritedFontFamily,
                .emptyText = tr("ui.controls.search-picker.empty"),
                .preferredHeight = 280.0F,
            };
          }
          entries.push_back(makeEntry(
              section, "widgets", tr("settings.schema.bar.font-family.label"),
              tr("settings.schema.bar.font-family.description"), monitorPath("font_family"),
              std::move(fontFamilyControl), "typeface font", true
          ));
        }
        appendRow(entries, ovrRows, "widget_spacing");
        appendRow(entries, inheritRows, "color");
        appendRow(entries, inheritRows, "icon_color");
        for (const auto* key : {"capsule", "capsule_thickness"}) {
          appendRow(entries, ovrRows, key);
        }
        const SettingVisibility monitorCapsuleOn =
            [on = ovr.widgetCapsuleDefault.value_or(bar.widgetCapsuleDefault)](const Config&) { return on; };
        appendRow(entries, inheritRows, "capsule_radius");
        for (const auto* key : {"capsule_fill", "capsule_foreground", "capsule_border"}) {
          appendRow(entries, inheritRows, key);
          entries.back().visibleWhen = monitorCapsuleOn;
        }
        for (const auto* key : {"capsule_border_width", "capsule_padding", "capsule_opacity"}) {
          appendRow(entries, ovrRows, key);
          entries.back().visibleWhen = monitorCapsuleOn;
        }
        for (const auto* key : {"start", "center", "end"}) {
          appendRow(entries, ovrRows, key);
        }
        auto ovrDeadZoneRows = generateSchemaEntries(
            noctalia::config::schema::barDeadZoneSchema(), resolved.deadZone,
            std::vector<std::string>{"bar", bar.name, "monitor", ovr.match, "dead_zone"}
        );
        appendRow(entries, ovrDeadZoneRows, "click_through");
        entries.back().visibleWhen = [monitorAutoHideOn](const Config& c) { return !monitorAutoHideOn(c); };
        for (const auto gesture : noctalia::bar::allGestures()) {
          const std::string key(noctalia::bar::gestureConfigKey(gesture));
          std::vector<std::string> gesturePath = root;
          gesturePath.emplace_back("dead_zone");
          gesturePath.emplace_back("actions");
          gesturePath.emplace_back(key);

          const auto& overrideActions = ovr.deadZone.actions;
          std::string configured;
          if (overrideActions.has_value()) {
            const auto overridden = overrideActions->find(key);
            if (overridden != overrideActions->end()) {
              configured = overridden->second;
            }
          }
          const auto inherited = bar.deadZone.actions.find(key);
          entries.push_back(makeEntry(
              section, deadZoneGroup(gesture), tr(noctalia::bar::gestureLabelKey(gesture)),
              tr("settings.schema.bar.dead-zone-action.description"), std::move(gesturePath),
              GestureActionSetting{
                  .gestureKey = key,
                  .configured = std::move(configured),
                  .defaultAction =
                      inherited != bar.deadZone.actions.end() ? inherited->second : deadZoneDefault(gesture),
              },
              "bar empty margin dead zone action command gesture " + key
          ));
        }
      }
    }
  }

} // namespace settings
