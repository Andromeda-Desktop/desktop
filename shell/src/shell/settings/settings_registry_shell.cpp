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

  void registry_detail::add_security_settings(RegistryContext& ctx) {
    using i18n::tr;
    const auto& cfg = ctx.cfg;
    const auto& env = ctx.env;
    auto& entries = ctx.entries;
    auto& shellRows = ctx.shellRows;

    // Security
    for (const auto* key : {"offline_mode", "polkit_agent", "password_style"}) {
      appendRow(entries, shellRows, key);
    }
    const SettingVisibility lockscreenOn = [](const Config& c) { return c.lockscreen.enabled; };
    auto lockscreenRows =
        generateSchemaEntries(noctalia::config::schema::lockscreenSchema(), cfg.lockscreen, {"lockscreen"}, &env);
    appendRow(entries, lockscreenRows, "enabled");
    const auto lockRow = [&](std::string_view key, const SettingVisibility& visibleWhen) {
      appendRow(entries, lockscreenRows, key);
      entries.back().visibleWhen = visibleWhen;
    };
    for (const auto* key : {"lock_before_suspend", "dnd", "grace_period_seconds", "fingerprint", "face"}) {
      lockRow(key, lockscreenOn);
    }
    lockRow("face_on_request", [](const Config& c) { return c.lockscreen.enabled && c.lockscreen.face; });
    lockRow("theme", lockscreenOn);
    {
      // Andromeda-only rows: Ryoku draws its own dial and ignores layout and clock options.
      const SettingVisibility andromedaOn = [](const Config& c) {
        return c.lockscreen.enabled && c.lockscreen.theme == "andromeda";
      };
      lockRow("layout", andromedaOn);
      for (const auto* key : {"clock_style", "clock_size", "clock_seconds", "notification_content", "owner_message"}) {
        lockRow(key, andromedaOn);
      }
    }
    lockRow("allow_empty_password", lockscreenOn);
    if (env.screencopySupported) {
      MultiSelectSetting transitions;
      transitions.options.reserve(std::size(kLockscreenTransitions));
      for (const auto& opt : kLockscreenTransitions) {
        transitions.options.push_back(SelectOption{std::string(opt.key), tr(opt.labelKey)});
      }
      transitions.selectedValues.reserve(cfg.lockscreen.transitions.size());
      for (const auto& transition : cfg.lockscreen.transitions) {
        transitions.selectedValues.emplace_back(enumToKey(kLockscreenTransitions, transition));
      }
      auto transitionEffects = makeEntry(
          SettingsSection::Security, "lock-screen", tr("settings.schema.lockscreen.transitions.label"),
          tr("settings.schema.lockscreen.transitions.description"), {"lockscreen", "transition"},
          std::move(transitions), "lock screen effects animation pool"
      );
      transitionEffects.visibleWhen = lockscreenOn;
      entries.push_back(std::move(transitionEffects));
      lockRow("transition_duration", lockscreenOn);
      lockRow("edge_smoothness", lockscreenOn);
    }
    {
      const SettingVisibility lockscreenWallpaperOn = [](const Config& c) {
        return c.lockscreen.enabled;
      };
      auto e = makeEntry(
          SettingsSection::Security, "lock-screen", tr("settings.schema.lockscreen.wallpaper.label"),
          tr("settings.schema.lockscreen.wallpaper.description"), {"lockscreen", "wallpaper"},
          TextSetting{
              .value = cfg.lockscreen.wallpaper,
              .placeholder = tr("settings.schema.lockscreen.wallpaper.placeholder"),
              .browseMode = TextSettingBrowseMode::OpenFile,
              .browseFileExtensions = DirectoryScanner::imageExtensionFilter(true),
              .browseFallbackDirectory = wallpaper::resolveGlobalWallpaperDirectory(
                  cfg.wallpaper,
                  wallpaper::effectiveThemeMode(
                      shellThemeMode(cfg.theme), shellThemeMode(cfg.theme) == ThemeMode::Light
                  )
              ),
          },
          "lock screen background image custom"
      );
      e.visibleWhen = lockscreenWallpaperOn;
      entries.push_back(std::move(e));
    }
    lockRow("blur_intensity", lockscreenOn);
    lockRow("tint_intensity", lockscreenOn);
    lockRow("monitors", lockscreenOn);
    {
      auto widgetRows = generateSchemaEntries(
          noctalia::config::schema::lockscreenWidgetsSchema(), cfg.lockscreenWidgets, {"lockscreen_widgets"}
      );
      appendRow(entries, widgetRows, "enabled");
      entries.back().visibleWhen = lockscreenOn;
    }
    auto privacyRows =
        generateSchemaEntries(noctalia::config::schema::privacySchema(), cfg.shell.privacy, {"shell", "privacy"});
    appendGroup(entries, privacyRows, "privacy");
    if (env.greeterSyncAvailable) {
      entries.push_back(makeEntry(
          SettingsSection::Security, "greeter", tr("settings.schema.shell.greeter-sync-privilege-command.label"),
          tr("settings.schema.shell.greeter-sync-privilege-command.description"),
          {"shell", "greeter_sync", "privilege_command"},
          TextSetting{
              .value = cfg.shell.greeterSync.privilegeCommand,
              .placeholder = "pkexec",
              .browseFileExtensions = {},
          },
          "greeter sync pkexec run0 ghostty terminal sudo polkit wrapper"
      ));
    }
  }

  void registry_detail::add_shell_settings(RegistryContext& ctx) {
    using i18n::tr;
    const auto& cfg = ctx.cfg;
    const auto& env = ctx.env;
    auto& entries = ctx.entries;
    auto& shellRows = ctx.shellRows;

    // Shell
    entries.push_back(makeEntry(
        SettingsSection::Shell, "general", tr("settings.schema.shell.avatar-path.label"),
        tr("settings.schema.shell.avatar-path.description"), {"shell", "avatar_path"},
        TextSetting{
            .value = env.shellAvatarPath,
            .placeholder = tr("settings.schema.shell.avatar-path.placeholder"),
            .browseMode = TextSettingBrowseMode::OpenFile,
            .browseFileExtensions = DirectoryScanner::imageExtensionFilter(true)
        },
        "image picture"
    ));
    for (const auto* key : {"settings_window_translucent", "settings_expand_all_groups", "time_format", "date_format",
                            "readline_shortcuts"}) {
      appendRow(entries, shellRows, key);
    }
    appendRow(entries, shellRows, "automount_drives");
    entries.push_back(makeEntry(
        SettingsSection::Shell, "keyboard-layout", tr("settings.schema.shell.keyboard-layout-custom-labels.label"),
        tr("settings.schema.shell.keyboard-layout-custom-labels.description"),
        {"shell", "keyboard_layout", "custom_labels"},
        StringMapSetting{
            .entries = cfg.shell.keyboardLayout.customLabels,
            .suggestedKeys = env.keyboardLayoutNames,
            .keyPlaceholder = tr("settings.widgets.map-placeholders.layout-name"),
            .valuePlaceholder = tr("settings.widgets.map-placeholders.value"),
        },
        "keyboard layout nickname alias label"
    ));
    appendRow(entries, shellRows, "launch_apps_as_systemd_services");
    appendRow(entries, shellRows, "launch_apps_custom_command");
    entries.back().visibleWhen = [managed = env.systemdUserManaged](const Config& c) {
      return !(c.shell.launchAppsAsSystemdServices && managed);
    };
    const SettingVisibility clipboardOn = [](const Config& c) { return c.shell.clipboardEnabled; };
    appendRow(entries, shellRows, "clipboard_enabled");
    // Deliberately not gated on clipboardOn: this keeps the live selection
    // alive, which is part of the transport that stays active when history
    // retention is turned off.
    appendRow(entries, shellRows, "clipboard_keep_from_closed_apps");
    for (const auto* key :
         {"clipboard_history_max_entries", "clipboard_confirm_clear_history", "clipboard_auto_paste",
          "clipboard_image_action_command"}) {
      appendRow(entries, shellRows, key);
      entries.back().visibleWhen = clipboardOn;
    }
    auto screenshotRows = generateSchemaEntries(
        noctalia::config::schema::screenshotSchema(), cfg.shell.screenshot, {"shell", "screenshot"}
    );
    for (const auto* key : {"freeze_screen", "confirm_region", "remember_last_region", "region_modifier", "show_cursor"}) {
      appendRow(entries, screenshotRows, key);
    }
    appendGroup(entries, screenshotRows, "screenshot-capture");
    appendGroup(entries, screenshotRows, "screenshot-annotation");
    for (const auto* key : {"copy_to_clipboard", "save_to_file", "directory", "filename_pattern", "format"}) {
      appendRow(entries, screenshotRows, key);
    }
    appendRow(entries, screenshotRows, "quality");
    entries.back().visibleWhen = [](const Config& c) { return c.shell.screenshot.format != ScreenshotFormat::Png; };
    appendRow(entries, screenshotRows, "pipe_to_command");
    appendRow(entries, screenshotRows, "pipe_command");
    entries.back().visibleWhen = [](const Config& c) { return c.shell.screenshot.pipeToCommand; };
    auto windowSwitcherRows = generateSchemaEntries(
        noctalia::config::schema::windowSwitcherSchema(), cfg.shell.windowSwitcher, {"shell", "window_switcher"}
    );
    appendGroup(entries, windowSwitcherRows, "window-switcher");
    // Keybinds
    auto keybindRows = generateSchemaEntries(noctalia::config::schema::keybindsSchema(), cfg.keybinds, {"keybinds"});
    for (const auto* key : {"validate", "cancel", "left", "right", "up", "down", "tab_previous", "tab_next", "copy", "save",
                            "delete"}) {
      appendRow(entries, keybindRows, key);
    }

    // Andromeda-specific integrations
    if (env.andromedaOverviewTypeToLaunchSupported) {
      appendRow(entries, shellRows, "andromeda_audio_provider_enabled");
    }
    if (env.andromedaOverviewCloseOnLaunchSupported) {
      appendRow(entries, shellRows, "andromeda_overview_close_on_launch_enabled");
    }
  }

} // namespace settings
