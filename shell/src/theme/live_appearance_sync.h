#pragma once

#include "protocols/glass_material.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

class SettingsControl;
class ShellWaylandConnection;

// The compositor's live appearance keys (`appearance.*` in its settings table).
struct LiveAppearance {
  std::string scheme;
  std::string accent; // "#rrggbb"; empty when unset
  // appearance.glass_style, its sliders and appearance.reduce_transparency.
  glass::Settings glass;
  bool increaseContrast = false;
  bool reduceMotion = false;
  // animation.enabled and animation.speed: the compositor owns them for every animation on the desktop.
  bool animationEnabled = true;
  float animationSpeed = 1.0F;
  // appearance.blur.enabled: with it the compositor draws the glass tints the shell sends (dsk_surface_v1 v9 tint).
  bool blurEnabled = false;
  // general.developer_diagnostics: slow main-loop passes write stall reports.
  bool developerDiagnostics = false;
  // appearance.title_bar.button_position: server-side title bars put their buttons on the left.
  bool titleBarButtonsLeft = false;

  bool operator==(const LiveAppearance&) const = default;
};

// Reads the live keys from the compositor's settings table; `value` returns "" for a key the table lacks.
[[nodiscard]] LiveAppearance readLiveAppearance(const std::function<std::string(std::string_view key)>& value);
// Hands animation.enabled, animation.speed and appearance.reduce_motion to MotionService.
void applyLiveMotion(const LiveAppearance& appearance);

// Keeps a dsk_settings_manager_v1 binding for the whole session on andromeda: reads the live appearance keys and
// writes the shell's scheme and accent. The user's choices go to the saved base keys; derived values (an auto scheme,
// the wallpaper accent) go only to the runtime layer "style", so the compositor never saves them as the user's.
class LiveAppearanceSync {
public:
  using ChangeCallback = std::function<void(const LiveAppearance&)>;
  enum class SchemeOrigin : std::uint8_t { Chosen, Derived };

  LiveAppearanceSync();
  ~LiveAppearanceSync();

  void initialize(ShellWaylandConnection& wayland, ChangeCallback onChange);
  // The shell's long-lived compositor settings connection; null before initialize().
  [[nodiscard]] SettingsControl* settingsControl() noexcept { return m_control.get(); }
  enum class AccentOrigin : std::uint8_t { Saved, Wallpaper };

  // Sets each key the compositor holds with a different value; before the first batch arrives, it waits for it. The
  // palette accent fills the saved appearance.accent only while it is unset and the accent origin is Saved. Pure black
  // (rulebook §4.3) goes to the "style" layer in the same commit, so compositor chrome and glass follow the theme.
  void publish(std::string_view scheme, SchemeOrigin origin, std::string_view paletteAccent, bool pureBlack);
  // Saved: the user's saved appearance.accent shows. Wallpaper: the wallpaper accent ("#rrggbb", empty until known)
  // masks it from the "style" layer.
  void setAccentOrigin(AccentOrigin origin, std::string_view wallpaperAccent);
  // A Focus scheme in the compositor's runtime layer "focus" (never saved there); empty clears the layer. While it is
  // on, publish() keeps the user's scheme and no scheme change counts as another client's choice.
  void setFocusScheme(std::string scheme);
  // The shell's corner scale in the compositor's runtime layer "style" (never saved there): window corners follow it.
  void setCornerScale(float scale);
  void setUiScale(float scale);
  // The shell's loaded language in the "style" layer: compositor chrome (overview menus) speaks it too.
  void setLanguage(std::string_view language);
  // Runs when another client changes the saved appearance.scheme; the shell adopts it.
  void setOnExternalScheme(std::function<void(std::string_view)> callback) { m_onExternalScheme = std::move(callback); }
  // Runs on every batch of settings and on every refused set, once the first batch has arrived.
  void setOnBatch(std::function<void(const SettingsControl&)> callback) { m_onBatch = std::move(callback); }
  // Sets one compositor setting; call it from the setOnBatch() callback or later.
  void set(const std::string& key, const std::string& value);

  [[nodiscard]] const LiveAppearance& state() const noexcept { return m_state; }

private:
  void onSettingsChanged();
  void flush();
  void sendLayers();
  void push(const std::string& key, const std::string& value, bool differs);
  void setStyleLayer(const std::string& key, const std::string& value);
  void dropShellSaves(const std::array<std::pair<std::string, std::string>, 3>& recipe);

  std::unique_ptr<SettingsControl> m_control;
  LiveAppearance m_state;
  std::string m_scheme;
  SchemeOrigin m_schemeOrigin = SchemeOrigin::Chosen;
  std::string m_layerDerivedScheme; // the derived scheme in the compositor's "style" layer now
  std::string m_focusScheme;
  std::string m_layerScheme; // the scheme in the compositor's "focus" layer now
  std::string m_cornerScale;
  std::string m_layerCornerScale; // the corner scale in the compositor's "style" layer now
  std::string m_uiScale;
  std::string m_layerUiScale; // the ui scale in the compositor's "style" layer now
  std::string m_pureBlack;
  std::string m_layerPureBlack; // pure black in the compositor's "style" layer now
  std::string m_language;
  std::string m_layerLanguage; // the language in the compositor's "style" layer now
  std::string m_accent;
  AccentOrigin m_accentOrigin = AccentOrigin::Saved;
  std::string m_wallpaperAccent;
  std::unordered_map<std::string, std::string> m_styleLayer; // accent and blur recipe in the "style" layer now
  bool m_droppedShellSaves = false;
  std::unordered_map<std::string, std::string> m_sent; // last value set per key, until the compositor has it
  ChangeCallback m_onChange;
  std::function<void(std::string_view)> m_onExternalScheme;
  std::function<void(const SettingsControl&)> m_onBatch;
};
