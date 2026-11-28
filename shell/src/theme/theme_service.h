#pragma once

#include "core/latest_request.h"
#include "config/config_types.h"
#include "core/timer_manager.h"
#include "theme/palette.h"
#include "ui/palette.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

class ConfigService;
class HttpClient;
class IpcService;

namespace noctalia::theme {

  class DesktopTheme;

  class ThemeService {
  public:
    using ChangeCallback = std::function<void()>;
    // Carries the resolved [theme].mode: the app-facing mode templates and the GTK color
    // scheme run in. Shell-facing consumers read resolvedShellMode()/isLightMode().
    using ResolvedCallback = std::function<void(const GeneratedPalette&, std::string_view)>;

    ThemeService(ConfigService& config, HttpClient& httpClient, const DesktopTheme& desktopTheme);
    ~ThemeService();

    ThemeService(const ThemeService&) = delete;
    ThemeService& operator=(const ThemeService&) = delete;

    // Snaps the palette to the resolved theme (no fade). Used at startup.
    void apply();

    // Resolves the target theme and cross-fades to it.
    void onConfigReload();
    void onAutoSchemeChanged();
    void setAutoCoordinates(std::optional<double> latitude, std::optional<double> longitude);
    void toggleLightDark();
    void cycleMode();
    [[nodiscard]] ThemeMode configuredMode() const noexcept;
    // Noctalia's own resolved mode ([theme].shell_mode, or [theme].mode when it follows).
    [[nodiscard]] bool isLightMode() const noexcept;
    [[nodiscard]] std::string_view resolvedShellMode() const noexcept;
    // The resolved [theme].mode, which drives apps.
    [[nodiscard]] std::string_view resolvedMode() const noexcept;

    // The compositor's live appearance: accent "#rrggbb" (empty keeps the token accent) and increased contrast.
    void setLiveAppearance(std::string accent, bool increaseContrast);
    // A Focus scheme ("light" | "dark") over both resolved modes, never saved; empty: the configured modes. A desktop
    // theme with one scheme wins over both.
    void setModeOverride(std::string mode);

    // Accent colors for GTK/Qt/KDE files in the resolved app mode: the target palette, not a mid-fade value.
    struct ToolkitColors {
      ::Color accent;
      ::Color accentText;
      ::Color onAccent;
      bool operator==(const ToolkitColors&) const = default;
    };
    [[nodiscard]] const ToolkitColors& toolkitColors() const noexcept { return m_toolkitColors; }

    void setChangeCallback(ChangeCallback callback);
    void setResolvedCallback(ResolvedCallback callback);

    void registerIpc(IpcService& ipc);

    // A look change goes through the compositor's frame freeze (dsk_shell_v1 freeze_frame): freeze returns false when
    // the compositor cannot freeze, and the look then changes at once. onFrameFrozen takes frame_frozen.
    using FreezeFrame = std::function<bool(std::uint32_t serial)>;
    using ThawFrame = std::function<void(std::uint32_t serial)>;
    void setFrameFreeze(FreezeFrame freeze, ThawFrame thaw);
    void onFrameFrozen(std::uint32_t serial);


  private:
    ToolkitColors m_toolkitColors{};
    std::string m_liveAccent;
    bool m_liveHighContrast = false;

    void resolveAndSet(bool animate);
    void queueResolvedCallback(const GeneratedPalette& generated, std::string_view mode);
    void flushResolvedCallback(bool defer);
    // Idle: no switch. Freezing: waits for frame_frozen of m_switchSerial. Applied: the look changed; thaw follows.
    enum class SwitchState : std::uint8_t { Idle, Freezing, Applied };
    void setSwitchState(SwitchState next);
    // Everything a switch shows at once, after frame_frozen.
    struct Look {
      Palette palette;
      Palette dark;
      Palette light;
      ToolkitColors toolkit;
      bool appLight = false;
      bool shellLight = false;
    };
    void switchTo(const Look& target);
    void applyLook(const Look& target);
    [[nodiscard]] bool lookChanges(const Look& target) const;
    void rescheduleAutoTimer();
    [[nodiscard]] bool hasAutoMode() const noexcept;

    ConfigService& m_config;
    HttpClient& m_httpClient;
    const DesktopTheme& m_desktopTheme;


    ChangeCallback m_changeCallback;
    ResolvedCallback m_resolvedCallback;
    // External template/hooks callbacks are delayed until the shell palette is
    // applied, and deferred callbacks must be able to drop stale resolves.
    std::optional<GeneratedPalette> m_pendingResolvedPalette;
    std::string m_pendingResolvedMode;
    RequestEpoch m_resolvedFlush;

    FreezeFrame m_freezeFrame;
    ThawFrame m_thawFrame;
    SwitchState m_switchState = SwitchState::Idle;
    std::uint32_t m_switchSerial = 0;
    Look m_switchTarget{};
    // The look on screen; empty before the first apply.
    std::optional<Look> m_shown;
    Timer m_switchTimer;
    bool m_isShellLightMode = false;
    std::string m_modeOverride;
    bool m_isLightMode = false;
    std::optional<double> m_autoLatitude;
    std::optional<double> m_autoLongitude;
    Timer m_autoTimer;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
  };

} // namespace noctalia::theme
