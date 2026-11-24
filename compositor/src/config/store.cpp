#include "config/store.h"

#include "config/fields.h"
#include "config/keybind_parse.h"
#include "config/managed_settings.h"
#include "core/log.h"
#include "andromeda_data_dir.h"
#include "protocols/xdg_dirs.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace andromeda {

  namespace {

    constexpr Logger kLog("config");

    // Moves the files of `umbriel/`, the config directory's old name, beside `config` once, when `config` is missing.
    void adoptUmbrielConfig(const std::filesystem::path& config) {
      const std::filesystem::path dir = config.parent_path();
      const std::filesystem::path old = dir.parent_path() / "umbriel";
      std::error_code error;
      if (std::filesystem::exists(config, error) || !std::filesystem::exists(old / "config.toml", error)) {
        return;
      }
      std::filesystem::create_directories(dir, error);
      for (const auto& entry : std::filesystem::directory_iterator(old, error)) {
        const std::filesystem::path target = dir / entry.path().filename();
        if (!std::filesystem::exists(target, error)) {
          std::filesystem::rename(entry.path(), target, error);
        }
      }
      std::filesystem::remove(old, error);
      kLog.info("moved {} to {}", old.string(), dir.string());
    }

    std::filesystem::path userConfigPath() {
      std::filesystem::path path(".config/andromeda/config.toml");
      if (const std::string config = xdg::configHome(); !config.empty()) {
        path = std::filesystem::path(config) / "andromeda/config.toml";
      }
      adoptUmbrielConfig(path);
      return path;
    }

    enum class ConfigPathKind {
      Missing,
      RegularFile,
      Unavailable,
    };

    struct ConfigPathProbe {
      ConfigPathKind kind = ConfigPathKind::Missing;
      std::error_code error;
    };

    ConfigPathProbe probeConfigPath(const std::filesystem::path& path) {
      std::error_code error;
      const std::filesystem::file_status status = std::filesystem::status(path, error);
      if (error) {
        if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory) {
          return {};
        }
        return {.kind = ConfigPathKind::Unavailable, .error = error};
      }
      if (status.type() == std::filesystem::file_type::not_found) {
        return {};
      }
      return {
          .kind = std::filesystem::is_regular_file(status) ? ConfigPathKind::RegularFile : ConfigPathKind::Unavailable,
          .error = {},
      };
    }

    std::vector<std::filesystem::path> defaultConfigCandidates() {
      std::vector<std::filesystem::path> candidates{userConfigPath()};
      for (const std::string& directory : xdg::configDirs()) {
        candidates.push_back(std::filesystem::path(directory) / "andromeda/config.toml");
      }

      candidates.push_back(std::filesystem::path(kDataDir) / "andromeda/config.toml");
      return candidates;
    }

    struct ConfigSelection {
      std::filesystem::path root;
      std::vector<std::filesystem::path> watchPaths;
      bool found = false;
    };

    ConfigSelection selectDefaultConfig(const std::vector<std::filesystem::path>& candidates) {
      ConfigSelection selection{};
      selection.watchPaths = candidates;
      for (const std::filesystem::path& candidate : candidates) {
        if (probeConfigPath(candidate).kind != ConfigPathKind::Missing) {
          selection.root = candidate;
          selection.found = true;
          return selection;
        }
      }
      selection.root = candidates.empty() ? userConfigPath() : candidates.front();
      if (selection.watchPaths.empty()) {
        selection.watchPaths.push_back(selection.root);
      }
      return selection;
    }

    // input.toml was the settings file before every setting shared one; take it over when it is still ours.
    void adoptLegacyInputSettings(const std::filesystem::path& configRoot) {
      const std::filesystem::path legacy = configRoot.parent_path() / "input.toml";
      const std::filesystem::path current = managedSettingsFile(configRoot);
      std::error_code error;
      if (std::filesystem::exists(current, error) || !std::filesystem::exists(legacy, error)) {
        return;
      }
      std::ifstream in(legacy);
      std::string firstLine;
      std::getline(in, firstLine);
      if (!firstLine.starts_with("# Written by Andromeda")) {
        return;
      }
      in.close();
      std::filesystem::rename(legacy, current, error);
      if (error) {
        kLog.warn("config: cannot move {} to {}: {}", legacy.string(), current.string(), error.message());
      }
    }

    // The built-in set, then the packaged config's [keybinds] when the user's own file replaces it: a shortcut added to
    // the packaged config reaches every user who has not bound that chord.
    std::vector<Keybind> baseKeybinds(const std::filesystem::path& root, bool layered) {
      Config base;
      base.keybinds = defaultKeybinds();
      const std::filesystem::path shipped = std::filesystem::path(kDataDir) / "andromeda/config.toml";
      std::error_code error;
      configStore().setLayeredKeybinds(false);
      if (!layered || root == shipped || !std::filesystem::is_regular_file(shipped, error)) {
        return base.keybinds;
      }
      try {
        readShippedKeybinds(toml::parse_file(shipped.string()), base);
        configStore().setLayeredKeybinds(true);
      } catch (const std::exception& exception) {
        kLog.warn("cannot read shortcuts from {}: {}", shipped.string(), exception.what());
      }
      return base.keybinds;
    }

    // Reset the store for a load of `rootPath`, then read it if it is a file. Without one, runtime overrides are still
    // read over the defaults, and the outcome stays Missing so the session keeps reporting that no file was found.
    ConfigParseOutcome parseInto(
        Config& out, toml::table& effective, const std::filesystem::path& rootPath,
        const std::vector<std::filesystem::path>& watchPaths
    ) {
      configStore().beginLoad(watchPaths);
      const std::filesystem::path settingsFile = managedSettingsFile(rootPath);
      configStore().addWatchPath(settingsFile);
      const ConfigPathProbe root = probeConfigPath(rootPath);
      std::error_code settingsError;
      if (root.kind == ConfigPathKind::Missing && !std::filesystem::exists(settingsFile, settingsError)) {
        if (configStore().overrides().empty()) {
          return ConfigParseOutcome::Missing;
        }
        return parseConfig(out, effective, nullptr) == ConfigParseOutcome::Loaded ? ConfigParseOutcome::Missing
                                                                                  : ConfigParseOutcome::Failed;
      }
      if (root.kind == ConfigPathKind::Unavailable) {
        const std::string reason = root.error ? root.error.message() : "not a regular file";
        emitDiag(
            ConfigDiagnostic::Severity::Error, nullptr,
            std::format("cannot inspect config file {}: {}", rootPath.string(), reason)
        );
        return ConfigParseOutcome::Failed;
      }
      return parseConfig(out, effective, &rootPath);
    }

    std::string
    errorLines(const std::vector<ConfigDiagnostic>& diagnostics, bool (*selected)(const ConfigDiagnostic&)) {
      std::string lines;
      for (const ConfigDiagnostic& diagnostic : diagnostics) {
        if (!selected(diagnostic)) {
          continue;
        }
        if (!lines.empty()) {
          lines += '\n';
        }
        const std::string location = diagnostic.location();
        lines += location.empty() ? diagnostic.message : location + ": " + diagnostic.message;
      }
      return lines;
    }

    bool sameDiagnostic(const ConfigDiagnostic& a, const ConfigDiagnostic& b) {
      return std::tie(a.severity, a.message, a.file, a.line, a.column)
          == std::tie(b.severity, b.message, b.file, b.line, b.column);
    }

  } // namespace

  void ConfigStore::beginLoad(const std::vector<std::filesystem::path>& watchPaths) {
    m_diagnostics.clear();
    m_missingIncludes = false;
    m_watchPaths.clear();
    for (const std::filesystem::path& path : watchPaths) {
      addWatchPath(path);
    }
  }

  void ConfigStore::addDiagnostic(ConfigDiagnostic diagnostic) { m_diagnostics.push_back(std::move(diagnostic)); }

  void ConfigStore::addWatchPath(std::filesystem::path path) {
    if (std::ranges::find(m_watchPaths, path) == m_watchPaths.end()) {
      m_watchPaths.push_back(std::move(path));
    }
  }

  void ConfigStore::sortDiagnostics() {
    // Stable so two diagnostics on the same key keep the order they were found
    // in. File-less entries (whole-config errors) sort first.
    std::ranges::stable_sort(m_diagnostics, [](const ConfigDiagnostic& a, const ConfigDiagnostic& b) {
      return std::tie(a.file, a.line, a.column) < std::tie(b.file, b.line, b.column);
    });
  }

  void applyLiveAppearance(Config& config) {
    if (config.appearance.accent) {
      config.colors.accentPrimary = *config.appearance.accent;
    }
    if (config.appearance.reduceTransparency) {
      config.appearance.blur.enabled = false;
    }
    if (config.appearance.increaseContrast) {
      config.appearance.borderWidth = std::max(config.appearance.borderWidth, 2);
    }
    if (!config.appearance.reduceMotion) {
      return;
    }
    auto& animation = config.animation;
    animation.windowsIn.style = "fade";
    animation.windowsOut.style = "fade";
    animation.windowsMove.enabled = false;
    animation.windowsResize.enabled = false;
    // A workspace switch crossfades in place instead of sliding (rulebook 7.8): both workspaces stay live while the
    // incoming one fades over the outgoing one.
    constexpr std::string_view kCrossfade = "andromeda-reduce-motion-crossfade";
    if (findEffectPreset(config.effects, kCrossfade) == nullptr) {
      config.effects.presets.push_back(EffectPreset{
          .name = std::string(kCrossfade),
          .kind = EffectKind::Animation,
          .shader = {.code = "vec4 animation(vec2 uv) {\n"
                             "  return mix(andromeda_sample(uv), andromeda_sample_incoming(uv), andromeda_clamped_progress);\n"
                             "}\n"},
      });
    }
    animation.workspaces.style = "reveal";
    animation.workspaces.effect = std::string(kCrossfade);
    animation.scratchpad.enabled = false;
  }

  ConfigReloadResult
  ConfigStore::commit(Config&& config, toml::table&& effective, std::filesystem::path rootPath, bool fileMissing) {
    applyLiveAppearance(config);
    applyAnimationSpeed(config.animation);
    // Computed before the move, and only after the first load: everything is new
    // the first time through.
    ConfigReloadResult result{
        .success = true,
        .change = m_generation == 0 ? ConfigChange::everything() : ConfigChange::between(m_config, config),
        .effects = m_generation == 0 ? ConfigEffects::everything() : ConfigEffects::between(m_config, config),
    };
    m_config = std::move(config);
    m_effective = std::move(effective);
    m_rootPath = std::move(rootPath);
    m_fileMissing = fileMissing;
    ++m_generation;
    return result;
  }

  void ConfigStore::setRootPath(std::filesystem::path path, bool explicitPath) {
    m_rootPath = std::move(path);
    m_explicitPath = explicitPath;
  }

  ConfigStore& configStore() {
    static ConfigStore store;
    return store;
  }

  const Config& config() { return configStore().config(); }

  const std::vector<ConfigDiagnostic>& configDiagnostics() { return configStore().diagnostics(); }

  const std::filesystem::path& configRootPath() { return configStore().rootPath(); }

  bool configHasMissingIncludes() { return configStore().missingIncludes(); }

  void ConfigStore::load(const char* explicitPath) {
    m_overrides.clear();
    ConfigSelection selection;
    if (explicitPath != nullptr) {
      m_implicitCandidates.clear();
      selection.root = std::filesystem::path(explicitPath);
      selection.watchPaths.push_back(selection.root);
    } else {
      m_implicitCandidates = defaultConfigCandidates();
      selection = selectDefaultConfig(m_implicitCandidates);
    }
    setRootPath(selection.root, explicitPath != nullptr);
    adoptLegacyInputSettings(selection.root);

    Config loaded;
    loaded.keybinds = baseKeybinds(selection.root, explicitPath == nullptr);
    toml::table effective;
    const ConfigParseOutcome outcome = parseInto(loaded, effective, selection.root, selection.watchPaths);
    const bool missing = outcome == ConfigParseOutcome::Missing;
    if (missing) {
      if (m_explicitPath) {
        emitDiag(
            ConfigDiagnostic::Severity::Error, nullptr,
            std::format("config file not found: {}", selection.root.string())
        );
      } else {
        kLog.info("no config file found: {}, using defaults", selection.root.string());
      }
    }
    sortDiagnostics();
    (void)commit(std::move(loaded), std::move(effective), selection.root, missing);
  }

  ConfigReloadResult ConfigStore::replace(const std::filesystem::path& explicitPath, std::string& errors) {
    // save variables modified by parseInto to be restored on error
    const bool missingIncludes = m_missingIncludes;
    auto diagnostics = m_diagnostics;
    auto watchPaths = m_watchPaths;

    ConfigSelection selection;

    selection.root = explicitPath;
    selection.watchPaths.push_back(selection.root);

    Config loaded;
    loaded.keybinds = baseKeybinds(selection.root, false);
    toml::table effective;
    const ConfigParseOutcome outcome = parseInto(loaded, effective, selection.root, selection.watchPaths);

    const bool missing = outcome == ConfigParseOutcome::Missing;

    if (missing) {
      emitDiag(
          ConfigDiagnostic::Severity::Error, nullptr, std::format("config file not found: {}", selection.root.string())
      );
    }
    sortDiagnostics();
    if (outcome != ConfigParseOutcome::Loaded) {
      const auto& diags = configDiagnostics();

      for (const auto& d : diags) {
        if (d.severity == andromeda::ConfigDiagnostic::Severity::Error) {
          const std::string loc = d.location();
          errors += loc.empty() ? d.message : loc + ": " + d.message + "\n";
        }
      }

      m_diagnostics = std::move(diagnostics);
      m_missingIncludes = missingIncludes;
      m_watchPaths = watchPaths;

      return {};
    }

    m_implicitCandidates.clear();
    setRootPath(selection.root, true);

    return commit(std::move(loaded), std::move(effective), selection.root, missing);
  }

  ConfigStore::FileStamp ConfigStore::stamp(const std::filesystem::path& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
      return {};
    }
    return {
        .exists = true,
        .device = st.st_dev,
        .inode = st.st_ino,
        .size = st.st_size,
        .modifiedNs = st.st_mtim.tv_sec * 1'000'000'000LL + st.st_mtim.tv_nsec,
        .changedNs = st.st_ctim.tv_sec * 1'000'000'000LL + st.st_ctim.tv_nsec,
    };
  }

  std::optional<ConfigReloadResult> ConfigStore::reloadIfChanged() {
    const bool unchanged = std::ranges::all_of(m_watchPaths, [this](const std::filesystem::path& path) {
      const auto it = std::ranges::find(m_loadedStamps, path, &std::pair<std::filesystem::path, FileStamp>::first);
      return it != m_loadedStamps.end() && it->second == stamp(path);
    });
    if (unchanged && !m_watchPaths.empty()) {
      return std::nullopt;
    }
    return reload();
  }

  ConfigReloadResult ConfigStore::reload() {
    std::vector<std::pair<std::filesystem::path, FileStamp>> stamps;
    for (const std::filesystem::path& path : m_watchPaths) {
      stamps.emplace_back(path, stamp(path));
    }
    ConfigSelection selection;
    if (m_explicitPath) {
      selection.root = m_rootPath;
      selection.watchPaths.push_back(selection.root);
    } else {
      selection = selectDefaultConfig(m_implicitCandidates);
    }

    Config loaded;
    loaded.keybinds = baseKeybinds(selection.root, !m_explicitPath);
    toml::table effective;
    const ConfigParseOutcome outcome = parseInto(loaded, effective, selection.root, selection.watchPaths);
    const bool missing = outcome == ConfigParseOutcome::Missing;
    if (m_explicitPath && missing) {
      emitDiag(
          ConfigDiagnostic::Severity::Error, nullptr, std::format("config file not found: {}", selection.root.string())
      );
    }
    sortDiagnostics();
    if (outcome != ConfigParseOutcome::Loaded) {
      if (!m_explicitPath && !selection.found && missing) {
        kLog.info("no config file found: {}, using defaults", selection.root.string());
        m_loadedStamps = std::move(stamps);
        return commit(std::move(loaded), std::move(effective), selection.root, true);
      }
      kLog.warn("config reload failed; keeping previous configuration");
      return {};
    }
    m_loadedStamps = std::move(stamps);
    return commit(std::move(loaded), std::move(effective), selection.root, false);
  }

  ConfigReloadResult
  ConfigStore::applyOverrides(toml::table overrides, bool rejectNewDiagnostics, std::string& errors) {
    // Restored when the overrides are refused, so the active configuration stays exactly as it was.
    const bool missingIncludes = m_missingIncludes;
    std::vector<ConfigDiagnostic> diagnostics = m_diagnostics;
    std::vector<std::filesystem::path> watchPaths = m_watchPaths;
    toml::table previous = std::exchange(m_overrides, std::move(overrides));

    ConfigSelection selection;
    if (m_explicitPath) {
      selection.root = m_rootPath;
      selection.watchPaths.push_back(selection.root);
    } else {
      selection = selectDefaultConfig(m_implicitCandidates);
    }

    Config loaded;
    loaded.keybinds = baseKeybinds(selection.root, !m_explicitPath);
    toml::table effective;
    const ConfigParseOutcome outcome = parseInto(loaded, effective, selection.root, selection.watchPaths);
    const bool missing = outcome == ConfigParseOutcome::Missing;
    if (m_explicitPath && missing) {
      emitDiag(
          ConfigDiagnostic::Severity::Error, nullptr, std::format("config file not found: {}", selection.root.string())
      );
    }
    sortDiagnostics();

    bool refused = outcome == ConfigParseOutcome::Failed || (missing && (m_explicitPath || selection.found));
    if (refused) {
      errors = errorLines(m_diagnostics, [](const ConfigDiagnostic& diagnostic) {
        return diagnostic.severity == ConfigDiagnostic::Severity::Error;
      });
    } else if (rejectNewDiagnostics) {
      // Each active diagnostic excuses one identical new one; whatever is left was introduced by the overrides.
      std::vector<ConfigDiagnostic> active = diagnostics;
      std::vector<ConfigDiagnostic> introduced;
      for (const ConfigDiagnostic& diagnostic : m_diagnostics) {
        const auto match = std::ranges::find_if(active, [&](const ConfigDiagnostic& existing) {
          return sameDiagnostic(existing, diagnostic);
        });
        if (match == active.end()) {
          introduced.push_back(diagnostic);
        } else {
          active.erase(match);
        }
      }
      refused = !introduced.empty();
      errors = errorLines(introduced, [](const ConfigDiagnostic&) { return true; });
    }
    if (refused) {
      if (errors.empty()) {
        errors = "config load failed";
      }
      m_overrides = std::move(previous);
      m_diagnostics = std::move(diagnostics);
      m_missingIncludes = missingIncludes;
      m_watchPaths = std::move(watchPaths);
      return {};
    }
    return commit(std::move(loaded), std::move(effective), selection.root, missing);
  }

  void loadConfig(const char* explicitPath) { configStore().load(explicitPath); }

  ConfigReloadResult replaceConfig(const std::filesystem::path& explicitPath, std::string& errors) {
    return configStore().replace(explicitPath, errors);
  }
  ConfigReloadResult reloadConfig() { return configStore().reload(); }
  std::optional<ConfigReloadResult> reloadConfigIfChanged() { return configStore().reloadIfChanged(); }

  const std::vector<std::filesystem::path>& configWatchPaths() { return configStore().watchPaths(); }

} // namespace andromeda
