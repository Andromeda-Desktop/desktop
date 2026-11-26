#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "render/text/font_weight_catalog.h"
#include "shell/profile/avatar_path.h"
#include "shell/settings/settings_window.h"
#include "system/day_night_schedule.h"
#include "compositors/settings_control.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

  bool settingPathNeedsSceneRebuild(const std::vector<std::string>& path) {
    if (path.size() == 2 && path[0] == "shell") {
      return path[1] == "corner_radius_scale"
          || path[1] == "font_family"
          || path[1] == "lang"
          || path[1] == "show_close_buttons"
          || path[1] == "settings_window_translucent";
    }
    if (path.size() == 2 && path[0] == "accessibility") {
      return path[1] == "ui_scale" || path[1] == "text_scale";
    }
    return false;
  }

  bool settingPathsNeedSceneRebuild(const std::vector<std::vector<std::string>>& paths) {
    return std::ranges::any_of(paths, [](const auto& path) { return settingPathNeedsSceneRebuild(path); });
  }

  std::string settingsMutationError(const ConfigService& config, const std::string& fallback) {
    return config.lastMutationError().empty() ? fallback : config.lastMutationError();
  }

  bool isCustomSchedulePath(const std::vector<std::string>& path) {
    return path.size() == 2
        && path[0] == "location"
        && (path[1] == "custom_schedule" || path[1] == "sunset" || path[1] == "sunrise");
  }

} // namespace

// A schedule with unusable times, or with no location source, schedules nothing. The write itself is valid, so
// surface it as a banner rather than rejecting it: the user is likely mid-edit between the toggle and the times.
void SettingsWindow::warnOnUnusableSchedule(const std::vector<std::string>& path) {
  if (m_config == nullptr) {
    return;
  }
  const Config& cfg = m_config->config();
  const LocationConfig& location = cfg.location;
  if (isCustomSchedulePath(path) && location.customSchedule && !day_night_schedule::hasUsableCustomTimes(location)) {
    showTransientStatus(i18n::tr("settings.errors.custom-schedule-times"), true);
  } else if (path == std::vector<std::string>{"brightness", "scheduled"} && cfg.brightness.scheduled
             && !location.customSchedule && !location.autoLocate && location.address.empty()
             && !(location.latitude && location.longitude)) {
    showTransientStatus(i18n::tr("settings.errors.brightness-schedule-location"), true);
  }
}

void SettingsWindow::markSettingsWriteSuccess(bool requestRebuild) {
  if (m_editorSheetModal != nullptr && m_editorSheetModal->isOpen()) {
    m_editorSheetModal->clearStatusMessage();
  }
  m_statusMessage.clear();
  m_statusIsError = false;
  m_pendingResetPageScope.clear();
  if (requestRebuild) {
    requestSceneRebuild();
  }
}

void SettingsWindow::markSettingsWriteError(std::string message) {
  if (m_editorSheetModal != nullptr && m_editorSheetModal->isOpen()) {
    m_editorSheetModal->setStatusMessage(std::move(message), true);
    return;
  }
  m_statusMessage = std::move(message);
  m_statusIsError = true;
  requestSceneRebuild();
}

void SettingsWindow::finishSettingsWrite(
    bool changed, bool forceSceneRebuild, bool pageResetPathsChanged, bool registryAlreadyCurrent,
    bool rebuildWhenUnchanged
) {
  const bool hadStatus = !m_statusMessage.empty();
  const bool hadPendingReset = !m_pendingResetPageScope.empty();
  markSettingsWriteSuccess(false);
  if (forceSceneRebuild || hadStatus || hadPendingReset) {
    requestSceneRebuild();
  } else if (changed || rebuildWhenUnchanged) {
    requestContentRebuild(
        changed ? !registryAlreadyCurrent : true, pageResetPathsChanged || rebuildWhenUnchanged, true
    );
  }
}

void SettingsWindow::showTransientStatus(std::string message, bool isError) {
  m_statusMessage = std::move(message);
  m_statusIsError = isError;
  requestSceneRebuild();
}

namespace {

  constexpr Logger kLog("settings");

  std::string compositorSettingText(const ConfigOverrideValue& value) {
    return std::visit(
        []<typename T>(const T& v) -> std::string {
          if constexpr (std::is_same_v<T, bool>) {
            return v ? "true" : "false";
          } else if constexpr (std::is_same_v<T, std::int64_t> || std::is_same_v<T, double>) {
            return std::format("{}", v);
          } else if constexpr (std::is_same_v<T, std::string>) {
            return v;
          } else {
            return {};
          }
        },
        value
    );
  }

} // namespace

bool SettingsWindow::commitCompositorSetting(const std::vector<std::string>& path, const std::string& value) {
  const auto key = settings::compositorSettingKey(path);
  if (!key.has_value()) {
    return false;
  }
  setCompositorSetting(*key, value);
  return true;
}

void SettingsWindow::setSettingOverride(std::vector<std::string> path, ConfigOverrideValue value) {
  if (commitCompositorSetting(path, compositorSettingText(value))) {
    return;
  }
  if (path.size() == 2 && path[0] == "shell" && path[1] == "font_family") {
    text::invalidateFontWeightCatalogCache();
  }
  const bool isAvatarPath = path.size() == 2 && path[0] == "shell" && path[1] == "avatar_path";
  DeferredCall::callLaterIfAlive(m_aliveGuard, [this, path = std::move(path), value = std::move(value), isAvatarPath] mutable {
    if (m_config == nullptr) {
      return;
    }
    if (isAvatarPath) {
      const auto* avatarPath = std::get_if<std::string>(&value);
      if (avatarPath == nullptr) {
        markSettingsWriteError(i18n::tr("settings.errors.write"));
        return;
      }
      shell::applyAvatarPath(m_accounts, m_config, *avatarPath, m_aliveGuard, [this](shell::AvatarApplyResult result) {
        if (result.success()) {
          markSettingsWriteSuccess();
          return;
        }
        markSettingsWriteError(i18n::tr(shell::avatarApplyErrorTranslationKey(result.error)));
      });
      return;
    }
    bool changed = false;
    const bool needsSceneRebuild = settingPathNeedsSceneRebuild(path);
    const ConfigOverrideValue patchValue = value;
    const auto previousResetPaths = currentPageResetPaths();
    if (m_config->setOverride(path, std::move(value), &changed)) {
      const bool registryPatched = changed && !needsSceneRebuild && tryPatchSettingsRegistryValue(path, patchValue);
      finishSettingsWrite(changed, needsSceneRebuild, previousResetPaths != currentPageResetPaths(), registryPatched);
      warnOnUnusableSchedule(path);
      return;
    }
    markSettingsWriteError(settingsMutationError(*m_config, i18n::tr("settings.errors.write")));
  });
}

void SettingsWindow::setSettingOverrides(
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides
) {
  std::vector<std::pair<std::string, std::string>> compositorWrites;
  std::erase_if(overrides, [&compositorWrites](const auto& overrideEntry) {
    const auto key = settings::compositorSettingKey(overrideEntry.first);
    if (key.has_value()) {
      compositorWrites.emplace_back(*key, compositorSettingText(overrideEntry.second));
    }
    return key.has_value();
  });
  if (m_config == nullptr) {
    return;
  }
  // The config part (e.g. theme.accent_source) commits first and synchronously, so a failed write leaves the
  // compositor untouched and no async reader sees the old config next to the new compositor value.
  bool changed = false;
  bool needsSceneRebuild = false;
  bool resetPathsChanged = false;
  bool registryPatched = false;
  if (!overrides.empty()) {
    needsSceneRebuild = std::ranges::any_of(overrides, [](const auto& overrideEntry) {
      return settingPathNeedsSceneRebuild(overrideEntry.first);
    });
    const auto patchOverrides = overrides;
    const auto previousResetPaths = currentPageResetPaths();
    if (!m_config->setOverrides(overrides, &changed)) {
      kLog.warn("settings: batch write failed; {} compositor setting(s) not written", compositorWrites.size());
      markSettingsWriteError(settingsMutationError(*m_config, i18n::tr("settings.errors.batch-write")));
      return;
    }
    registryPatched = changed && !needsSceneRebuild && tryPatchSettingsRegistryOverrides(patchOverrides);
    resetPathsChanged = previousResetPaths != currentPageResetPaths();
  }
  for (const auto& [key, value] : compositorWrites) {
    setCompositorSetting(key, value);
  }
  if (overrides.empty()) {
    DeferredCall::callLaterIfAlive(m_aliveGuard, [this] { markSettingsWriteSuccess(!m_statusMessage.empty()); });
    return;
  }
  DeferredCall::callLaterIfAlive(m_aliveGuard, [this, changed, needsSceneRebuild, resetPathsChanged, registryPatched] {
    finishSettingsWrite(changed, needsSceneRebuild, resetPathsChanged, registryPatched);
  });
}

void SettingsWindow::clearSettingOverride(std::vector<std::string> path) {
  if (commitCompositorSetting(path, {})) {
    return;
  }
  DeferredCall::callLaterIfAlive(m_aliveGuard, [this, path = std::move(path)]() mutable {
    if (m_config == nullptr) {
      return;
    }
    bool changed = false;
    const bool needsSceneRebuild = settingPathNeedsSceneRebuild(path);
    const auto previousResetPaths = currentPageResetPaths();
    if (!m_config->clearOverrides({path}, &changed)) {
      markSettingsWriteError(i18n::tr("settings.errors.write"));
      return;
    }

    const std::vector<std::vector<std::string>> paths{path};
    const bool registryPatched = changed && !needsSceneRebuild && tryPatchSettingsRegistryResetValues(paths);
    finishSettingsWrite(
        changed, needsSceneRebuild, previousResetPaths != currentPageResetPaths(), registryPatched, true
    );
  });
}

void SettingsWindow::clearSettingOverrides(std::vector<std::vector<std::string>> paths) {
  const bool compositorOnly = !paths.empty()
      && std::ranges::all_of(paths, [](const auto& path) { return settings::compositorSettingKey(path).has_value(); });
  std::erase_if(paths, [this](const std::vector<std::string>& path) { return commitCompositorSetting(path, {}); });
  if (compositorOnly) {
    markSettingsWriteSuccess(/*requestRebuild=*/true);
    return;
  }
  DeferredCall::callLaterIfAlive(m_aliveGuard, [this, paths = std::move(paths)]() mutable {
    if (m_config == nullptr || paths.empty()) {
      return;
    }

    bool changed = false;
    const bool needsSceneRebuild = settingPathsNeedSceneRebuild(paths);
    const auto previousResetPaths = currentPageResetPaths();
    const bool success = m_config->clearOverrides(paths, &changed);
    m_pendingResetPageScope.clear();
    if (!success) {
      markSettingsWriteError(i18n::tr("settings.errors.reset-page"));
      return;
    }

    const bool registryPatched = changed && !needsSceneRebuild && tryPatchSettingsRegistryResetValues(paths);
    finishSettingsWrite(
        changed, needsSceneRebuild, previousResetPaths != currentPageResetPaths(), registryPatched, true
    );
  });
}

void SettingsWindow::mutateSettingOverrides(
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides,
    std::vector<std::vector<std::string>> clears, std::optional<OverrideStash> stash
) {
  // The compositor is written only after the config commit succeeds; its clears go first, so a set wins.
  std::vector<std::pair<std::string, std::string>> compositorWrites;
  for (const auto& path : clears) {
    if (const auto key = settings::compositorSettingKey(path)) {
      compositorWrites.emplace_back(*key, std::string{});
    }
  }
  std::erase_if(clears, [](const auto& path) { return settings::compositorSettingKey(path).has_value(); });
  std::erase_if(overrides, [&compositorWrites](const auto& overrideEntry) {
    const auto key = settings::compositorSettingKey(overrideEntry.first);
    if (key.has_value()) {
      compositorWrites.emplace_back(*key, compositorSettingText(overrideEntry.second));
    }
    return key.has_value();
  });
  if (m_config == nullptr) {
    return;
  }
  bool changed = false;
  const bool needsSceneRebuild = settingPathsNeedSceneRebuild(clears)
      || std::ranges::any_of(overrides, [](const auto& overrideEntry) {
           return settingPathNeedsSceneRebuild(overrideEntry.first);
         });
  const auto previousResetPaths = currentPageResetPaths();
  if ((!overrides.empty() || !clears.empty() || stash.has_value())
      && !m_config->mutateOverrides(overrides, clears, &changed, stash ? &*stash : nullptr)) {
    kLog.warn("settings: grouped write failed; {} compositor setting(s) not written", compositorWrites.size());
    markSettingsWriteError(settingsMutationError(*m_config, i18n::tr("settings.errors.batch-write")));
    return;
  }
  for (const auto& [key, value] : compositorWrites) {
    setCompositorSetting(key, value);
  }
  const bool resetPathsChanged = previousResetPaths != currentPageResetPaths();
  DeferredCall::callLaterIfAlive(m_aliveGuard, [this, changed, needsSceneRebuild, resetPathsChanged] {
    finishSettingsWrite(changed, needsSceneRebuild, resetPathsChanged, false);
  });
}

void SettingsWindow::resetBarLane(std::vector<std::string> lanePath) {
  DeferredCall::callLaterIfAlive(m_aliveGuard, [this, lanePath = std::move(lanePath)] mutable {
    if (m_config == nullptr) {
      return;
    }
    bool changed = false;
    const bool needsSceneRebuild = settingPathNeedsSceneRebuild(lanePath);
    const auto previousResetPaths = currentPageResetPaths();
    if (!m_config->resetBarLaneOverride(lanePath, &changed)) {
      markSettingsWriteError(i18n::tr("settings.errors.write"));
      return;
    }

    const std::vector<std::vector<std::string>> paths{lanePath};
    const bool registryPatched = changed && !needsSceneRebuild && tryPatchSettingsRegistryResetValues(paths);
    finishSettingsWrite(
        changed, needsSceneRebuild, previousResetPaths != currentPageResetPaths(), registryPatched, true
    );
  });
}

void SettingsWindow::renameWidgetInstance(
    std::string oldName, std::string newName,
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> referenceOverrides
) {
  DeferredCall::callLaterIfAlive(m_aliveGuard, [this, oldName = std::move(oldName), newName = std::move(newName),
                           referenceOverrides = std::move(referenceOverrides)] mutable {
    if (m_config == nullptr) {
      return;
    }

    bool changed = m_config->renameOverrideTable({"widget", oldName}, {"widget", newName});
    if (!changed) {
      markSettingsWriteError(i18n::tr("settings.errors.widget.rename"));
      return;
    }
    bool failed = false;
    for (auto& [path, value] : referenceOverrides) {
      if (m_config->setOverride(path, std::move(value))) {
        changed = true;
      } else {
        failed = true;
      }
    }
    if (failed) {
      markSettingsWriteError(i18n::tr("settings.errors.batch-write"));
      return;
    }
    markSettingsWriteSuccess(changed);
  });
}

void SettingsWindow::createBar(std::string name) {
  DeferredCall::callLaterIfAlive(m_aliveGuard, [this, name = std::move(name)] {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->createBarOverride(name)) {
      navigateTo("bar", name);
      m_creatingBarName.clear();
      m_renamingBarName.clear();
      m_renamingMonitorOverrideBarName.clear();
      m_renamingMonitorOverrideMatch.clear();
      m_contentScrollState.offset = 0.0F;
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.bar.create"));
  });
}

void SettingsWindow::renameBar(std::string oldName, std::string newName) {
  DeferredCall::callLaterIfAlive(m_aliveGuard, [this, oldName = std::move(oldName), newName = std::move(newName)] {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->renameBarOverride(oldName, newName)) {
      if (m_selectedBarName == oldName) {
        m_selectedBarName = newName;
      }
      m_selectedMonitorOverride.clear();
      m_renamingBarName.clear();
      m_renamingMonitorOverrideBarName.clear();
      m_renamingMonitorOverrideMatch.clear();
      m_contentScrollState.offset = 0.0F;
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.bar.rename"));
  });
}

void SettingsWindow::deleteBar(std::string name) {
  DeferredCall::callLaterIfAlive(m_aliveGuard, [this, name = std::move(name)] {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->deleteBarOverride(name)) {
      if (m_selectedBarName == name) {
        m_selectedBarName.clear();
        m_selectedMonitorOverride.clear();
        m_contentScrollState.offset = 0.0F;
      }
      m_renamingBarName.clear();
      m_renamingMonitorOverrideBarName.clear();
      m_renamingMonitorOverrideMatch.clear();
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.bar.delete"));
  });
}

void SettingsWindow::moveBar(std::string name, int direction) {
  DeferredCall::callLaterIfAlive(m_aliveGuard, [this, name = std::move(name), direction] {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->moveBarOverride(name, direction)) {
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.bar.move"));
  });
}

void SettingsWindow::createMonitorOverride(std::string barName, std::string match) {
  DeferredCall::callLaterIfAlive(m_aliveGuard, [this, barName = std::move(barName), match = std::move(match)] {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->createMonitorOverride(barName, match)) {
      navigateTo("bar", barName, false, {}, match);
      m_renamingMonitorOverrideBarName.clear();
      m_renamingMonitorOverrideMatch.clear();
      m_contentScrollState.offset = 0.0F;
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.monitor-override.create"));
  });
}

void SettingsWindow::renameMonitorOverride(std::string barName, std::string oldMatch, std::string newMatch) {
  DeferredCall::callLaterIfAlive(m_aliveGuard, [this, barName = std::move(barName), oldMatch = std::move(oldMatch),
                           newMatch = std::move(newMatch)] {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->renameMonitorOverride(barName, oldMatch, newMatch)) {
      if (m_selectedBarName == barName && m_selectedMonitorOverride == oldMatch) {
        m_selectedMonitorOverride = newMatch;
      }
      m_renamingMonitorOverrideBarName.clear();
      m_renamingMonitorOverrideMatch.clear();
      m_contentScrollState.offset = 0.0F;
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.monitor-override.rename"));
  });
}

void SettingsWindow::deleteMonitorOverride(std::string barName, std::string match) {
  DeferredCall::callLaterIfAlive(m_aliveGuard, [this, barName = std::move(barName), match = std::move(match)] {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->deleteMonitorOverride(barName, match)) {
      if (m_selectedBarName == barName && m_selectedMonitorOverride == match) {
        m_selectedMonitorOverride.clear();
        m_contentScrollState.offset = 0.0F;
      }
      m_renamingMonitorOverrideBarName.clear();
      m_renamingMonitorOverrideMatch.clear();
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.monitor-override.delete"));
  });
}

void SettingsWindow::createDockMonitorOverride(std::string match) {
  DeferredCall::callLaterIfAlive(m_aliveGuard, [this, match = std::move(match)] {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->createDockMonitorOverride(match)) {
      navigateTo("desktop-dock", {}, false, {}, match);
      m_renamingDockMonitorOverride.clear();
      m_contentScrollState.offset = 0.0F;
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.monitor-override.create"));
  });
}

void SettingsWindow::renameDockMonitorOverride(std::string oldTableName, std::string newMatch) {
  DeferredCall::callLaterIfAlive(m_aliveGuard, [this, oldTableName = std::move(oldTableName), newMatch = std::move(newMatch)] {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->renameDockMonitorOverride(oldTableName, newMatch)) {
      if (m_selectedSection == "desktop-dock" && m_selectedMonitorOverride == oldTableName) {
        m_selectedMonitorOverride = newMatch;
      }
      m_renamingDockMonitorOverride.clear();
      m_contentScrollState.offset = 0.0F;
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.monitor-override.rename"));
  });
}

void SettingsWindow::deleteDockMonitorOverride(std::string tableName) {
  DeferredCall::callLaterIfAlive(m_aliveGuard, [this, tableName = std::move(tableName)] {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->deleteDockMonitorOverride(tableName)) {
      if (m_selectedSection == "desktop-dock" && m_selectedMonitorOverride == tableName) {
        m_selectedMonitorOverride.clear();
        m_contentScrollState.offset = 0.0F;
      }
      m_renamingDockMonitorOverride.clear();
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.monitor-override.delete"));
  });
}
