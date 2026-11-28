#include "theme/theme_service.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/scoped_timer.h"
#include "render/scene/node.h"
#include "ipc/ipc_service.h"
#include "net/http_client.h"
#include "system/day_night_schedule.h"
#include "theme/builtin_palettes.h"
#include "theme/desktop_theme.h"
#include "theme/fixed_palette.h"
#include "theme/palette_transform.h"
#include "theme/shell_palette.h"
#include "ui/app_icon_colorization.h"
#include "util/string_utils.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace noctalia::theme {

  namespace {

    constexpr auto kLog = Logger("theme");

    // One 60 Hz frame: a faster tick repaints every surface more often than the screen can show it.
    // The shell's surfaces redraw within two frames of a palette change; apps take the portal table at the same time.
    constexpr std::chrono::milliseconds kThawDelay{100};
    // frame_frozen comes within 200 ms of freeze_frame (dsk_shell_v1); a lost connection never answers.
    constexpr std::chrono::milliseconds kFreezeTimeout{400};

    struct ResolvedPalette {
      GeneratedPalette generated;
      Palette palette;
      // Resolved [theme].mode (apps) and the mode Noctalia itself runs in.
      std::string mode;
      std::string shellMode;
    };

    ResolvedPalette resolveTokens(std::string_view mode, std::string_view shellMode) {
      return {
          .generated = expandBuiltinPalette(andromedaPalette()),
          .palette = tokenPalette(shellMode == "light"),
          .mode = std::string(mode),
          .shellMode = std::string(shellMode),
      };
    }

  } // namespace

  ThemeService::ThemeService(ConfigService& config, HttpClient& httpClient, const DesktopTheme& desktopTheme)
      : m_config(config), m_httpClient(httpClient), m_desktopTheme(desktopTheme) {}

  ThemeService::~ThemeService() = default;

  namespace {

    // Applies the compositor accent and high contrast.
    void applyLivePalette(::Palette& palette, std::string_view accent, bool highContrast) {
      ::Color color;
      if (!accent.empty() && tryParseHexColor(accent, color)) {
        palette.accent = color;
        palette.onAccent = onAccentOn(readableTextColorForBackground(color), color);
        palette.accentText = accentTextOn(color, palette.solidRegular);
      }
      if (highContrast) {
        applyHighContrast(palette);
      }
    }

  } // namespace

  void ThemeService::setLiveAppearance(std::string accent, bool increaseContrast) {
    if (accent == m_liveAccent && increaseContrast == m_liveHighContrast) {
      return;
    }
    m_liveAccent = std::move(accent);
    m_liveHighContrast = increaseContrast;
    resolveAndSet(/*animate=*/true);
  }

  void ThemeService::apply() { resolveAndSet(/*animate=*/false); }

  void ThemeService::onConfigReload() { resolveAndSet(/*animate=*/true); }

  void ThemeService::onAutoSchemeChanged() {
    if (hasAutoMode()) {
      resolveAndSet(/*animate=*/true);
    }
  }

  void ThemeService::setAutoCoordinates(std::optional<double> latitude, std::optional<double> longitude) {
    if (latitude.has_value() && !std::isfinite(*latitude)) {
      latitude.reset();
    }
    if (longitude.has_value() && !std::isfinite(*longitude)) {
      longitude.reset();
    }
    if (m_autoLatitude == latitude && m_autoLongitude == longitude) {
      return;
    }
    m_autoLatitude = latitude;
    m_autoLongitude = longitude;
    if (hasAutoMode()) {
      resolveAndSet(/*animate=*/true);
    }
  }

  void ThemeService::toggleLightDark() {
    const auto next = m_isLightMode ? ThemeMode::Dark : ThemeMode::Light;
    // Persist via ConfigService → StateService. The resulting overrides-change
    // callback rebuilds the Config and fires the reload callbacks, which call
    // ThemeService::onConfigReload() to transition to the new palette.
    m_config.setThemeMode(next);
  }

  void ThemeService::cycleMode() {
    ThemeMode next = ThemeMode::Dark;
    switch (configuredMode()) {
    case ThemeMode::Dark:
      next = ThemeMode::Light;
      break;
    case ThemeMode::Light:
      next = ThemeMode::Auto;
      break;
    case ThemeMode::Auto:
      next = ThemeMode::Dark;
      break;
    }
    m_config.setThemeMode(next);
  }

  ThemeMode ThemeService::configuredMode() const noexcept { return m_config.config().theme.mode; }

  bool ThemeService::isLightMode() const noexcept { return m_isShellLightMode; }

  std::string_view ThemeService::resolvedShellMode() const noexcept { return m_isShellLightMode ? "light" : "dark"; }

  std::string_view ThemeService::resolvedMode() const noexcept { return m_isLightMode ? "light" : "dark"; }

  void ThemeService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

  void ThemeService::setResolvedCallback(ResolvedCallback callback) { m_resolvedCallback = std::move(callback); }

  void ThemeService::queueResolvedCallback(const GeneratedPalette& generated, std::string_view mode) {
    if (!m_resolvedCallback) {
      return;
    }
    m_pendingResolvedPalette = generated;
    m_pendingResolvedMode = std::string(mode);
    m_resolvedFlush.advance();
  }

  void ThemeService::flushResolvedCallback(bool defer) {
    if (!m_resolvedCallback || !m_pendingResolvedPalette.has_value()) {
      return;
    }

    GeneratedPalette generated = std::move(*m_pendingResolvedPalette);
    std::string mode = std::move(m_pendingResolvedMode);
    const LatestRequest::Ticket ticket = m_resolvedFlush.ticket();
    m_pendingResolvedPalette.reset();
    m_pendingResolvedMode.clear();

    if (defer) {
      DeferredCall::callLaterIfAlive(m_alive, [this, ticket, generated = std::move(generated), mode = std::move(mode)] mutable {
        if (m_resolvedFlush.current(ticket) && m_resolvedCallback) {
          m_resolvedCallback(generated, mode);
        }
      });
      return;
    }

    m_resolvedCallback(generated, mode);
  }

  void ThemeService::setModeOverride(std::string mode) {
    if (mode == m_modeOverride) {
      return;
    }
    m_modeOverride = std::move(mode);
    resolveAndSet(true);
  }

  void ThemeService::resolveAndSet(bool animate) {
    profiling::ScopedTimer t(kLog, "theme: resolveAndSet");
    const auto& cfg = m_config.config().theme;
    // [theme].mode is the app-facing mode; the shell either follows it or runs its own.
    // Both resolve `auto` against one schedule evaluation.
    std::optional<bool> scheduleNight;
    const auto modeName = [&](ThemeMode themeMode) -> std::string {
      if (themeMode != ThemeMode::Auto) {
        return themeMode == ThemeMode::Light ? "light" : "dark";
      }
      if (!scheduleNight.has_value()) {
        scheduleNight = day_night_schedule::evaluate(m_config.config().location, m_autoLatitude, m_autoLongitude).night;
      }
      return *scheduleNight ? "dark" : "light";
    };
    const auto pinned = m_desktopTheme.pinnedScheme();
    const auto resolveMode = [&](ThemeMode themeMode) {
      if (pinned) {
        return std::string(*pinned);
      }
      return m_modeOverride.empty() ? modeName(themeMode) : m_modeOverride;
    };
    const std::string mode = resolveMode(cfg.mode);
    const std::string shellMode = resolveMode(shellThemeMode(cfg));

    std::optional<ResolvedPalette> resolved = resolveTokens(mode, shellMode);
    if (cfg.pureBlackDark) {
      applyPureBlackDark(resolved->generated);
    }
    const bool highContrast = m_config.config().accessibility.highContrast || m_liveHighContrast;
    if (highContrast) {
      applyHighContrast(resolved->generated);
    }
    // The compositor's accent echoes the shell's style layer; only the wallpaper accent reaches the shell through it.
    // A theme or custom accent is per scheme in the tokens, so the echo of the previous scheme never shows.
    const std::string_view liveAccent = cfg.accentSource == AccentSource::Wallpaper ? m_liveAccent : std::string_view{};
    const auto paletteFor = [&](bool light) {
      ::Palette p = tokenPalette(light);
      if (!light && cfg.pureBlackDark) {
        applyPureBlack(p);
      }
      applyLivePalette(p, liveAccent, highContrast);
      return p;
    };
    resolved->palette = paletteFor(resolved->shellMode == "light");
    const ::Palette dark = resolved->shellMode == "light" ? paletteFor(false) : resolved->palette;
    const ::Palette light = resolved->shellMode == "light" ? resolved->palette : paletteFor(true);
    // Surfaces of the other tone (Clear glass, a dark island in light mode) read these, not the active palette.
    const bool tonesChanged =
        !(paletteForTone(PaletteTone::Dark) == dark) || !(paletteForTone(PaletteTone::Light) == light);
    setDarkPalette(dark);
    setLightPalette(light);
    const ::Palette appPalette = resolved->mode == resolved->shellMode ? resolved->palette : paletteFor(resolved->mode == "light");
    m_toolkitColors = {.accent = appPalette.accent, .accentText = appPalette.accentText, .onAccent = appPalette.onAccent};

    queueResolvedCallback(resolved->generated, resolved->mode);
    m_isLightMode = resolved->mode == "light";
    m_isShellLightMode = resolved->shellMode == "light";
    if (tonesChanged && m_switchState == SwitchState::Idle && palette == resolved->palette) {
      paletteChanged().emit();
      if (m_changeCallback) {
        m_changeCallback();
      }
    }

    if (animate) {
      switchTo(resolved->palette);
    } else {
      setSwitchState(SwitchState::Idle);
      if (lookChanges(resolved->palette)) {
        applyPalette(resolved->palette);
      } else {
        flushResolvedCallback(/*defer=*/false);
      }
    }
    rescheduleAutoTimer();
  }

  void ThemeService::rescheduleAutoTimer() {
    m_autoTimer.stop();
    if (!hasAutoMode()) {
      return;
    }
    constexpr auto kAutoRecheckInterval = std::chrono::minutes(1);
    const auto nextBoundary =
        day_night_schedule::evaluate(m_config.config().location, m_autoLatitude, m_autoLongitude).untilBoundary;
    const auto delay =
        std::min(nextBoundary, std::chrono::duration_cast<std::chrono::milliseconds>(kAutoRecheckInterval));
    m_autoTimer.start(delay, [this] { onAutoSchemeChanged(); });
  }

  bool ThemeService::hasAutoMode() const noexcept {
    const auto& theme = m_config.config().theme;
    return theme.mode == ThemeMode::Auto || shellThemeMode(theme) == ThemeMode::Auto;
  }

  void ThemeService::setFrameFreeze(FreezeFrame freeze, ThawFrame thaw) {
    m_freezeFrame = std::move(freeze);
    m_thawFrame = std::move(thaw);
  }

  void ThemeService::setSwitchState(SwitchState next) {
    m_switchTimer.stop();
    m_switchState = next;
    if (next == SwitchState::Freezing) {
      m_switchTimer.start(kFreezeTimeout, [this] {
        kLog.warn("theme: no frame_frozen {} within {} ms; switching without a fade", m_switchSerial, kFreezeTimeout.count());
        onFrameFrozen(m_switchSerial);
      });
    } else if (next == SwitchState::Applied) {
      m_switchTimer.start(kThawDelay, [this] {
        if (m_thawFrame) {
          m_thawFrame(m_switchSerial);
        }
        setSwitchState(SwitchState::Idle);
      });
    }
  }

  void ThemeService::switchTo(const Palette& target) {
    m_switchTarget = target;
    if (m_switchState == SwitchState::Freezing) {
      return;
    }
    if (!lookChanges(target)) {
      flushResolvedCallback(/*defer=*/false);
      return;
    }
    if (m_freezeFrame && m_freezeFrame(m_switchSerial + 1)) {
      ++m_switchSerial;
      setSwitchState(SwitchState::Freezing);
      return;
    }
    applyPalette(target);
  }

  bool ThemeService::lookChanges(const Palette& target) const {
    return !(palette == target) || !m_appliedShellLight.has_value() || *m_appliedShellLight != m_isShellLightMode;
  }

  void ThemeService::onFrameFrozen(std::uint32_t serial) {
    if (m_switchState != SwitchState::Freezing || serial != m_switchSerial) {
      return;
    }
    applyPalette(m_switchTarget);
    setSwitchState(SwitchState::Applied);
  }

  void ThemeService::applyPalette(const Palette& target) {
    setResolvedThemeLight(m_isShellLightMode);
    m_appliedShellLight = m_isShellLightMode;
    notifyShellAppIconColorizationChanged();
    setPalette(target);
    if (m_changeCallback) {
      m_changeCallback();
    }
    flushResolvedCallback(/*defer=*/false);
  }

  void ThemeService::registerIpc(IpcService& ipc) {
    ipc.bind(noctalia::cli::msg::themeModeToggle, [this](const std::string&) -> std::string {
      toggleLightDark();
      return "ok\n";
    });
    ipc.bind(
        noctalia::cli::msg::themeModeGet,
        [this](const std::string&) -> std::string {
          std::string out(resolvedMode());
          out.push_back('\n');
          return out;
        },
        IpcService::HandlerOptions{.actionEditorVisibility = IpcService::ActionEditorVisibility::Hidden}
    );
    ipc.bind(noctalia::cli::msg::themeModeSet, [this](const std::string& args) -> std::string {
      const std::string token = StringUtils::trim(args);
      const auto mode = enumFromKey(kThemeModes, token);
      if (!mode.has_value()) {
        return "error: expected dark, light, or auto\n";
      }
      m_config.setThemeMode(*mode);
      return "ok\n";
    });
  }

} // namespace noctalia::theme
