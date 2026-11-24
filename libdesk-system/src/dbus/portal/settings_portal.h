#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

class SessionBus;

namespace sdbus {
  class IObject;
  class Variant;
}

// Serves org.freedesktop.impl.portal.Settings under org.freedesktop.impl.portal.desktop.andromeda.shell, so apps follow
// the shell's dark mode through xdg-desktop-portal (org.freedesktop.appearance color-scheme).
class SettingsPortal {
public:
  explicit SettingsPortal(SessionBus& bus);
  ~SettingsPortal();

  SettingsPortal(const SettingsPortal&) = delete;
  SettingsPortal& operator=(const SettingsPortal&) = delete;

  // Emits SettingChanged only when the value changes.
  void setDark(bool dark);
  // org.freedesktop.appearance accent-color (sRGB 0..1), contrast (high = 1), reduced-motion (reduce = 1).
  void setAccent(double r, double g, double b);
  void setHighContrast(bool high);
  void setReducedMotion(bool reduce);
  // org.andromeda.appearance ui-scale and text-scale (factors, 1 = default): apps scale their windows and text;
  // corner-radius-scale (0..2, 1 = default) scales their corner radii.
  void setScales(double uiScale, double textScale, double cornerRadiusScale);
  // org.andromeda.appearance language: the shell's language tag; empty means each app reads its locale environment.
  void setLanguage(const std::string& language);
  // org.andromeda.appearance glass, frost and liquid (0..1), reduce-transparency and compositor-blur (1 = on): apps
  // take the same glass material as the shell.
  void setGlass(double glass, double frost, double liquid, bool reduceTransparency, bool compositorBlur);
  // org.andromeda.appearance glass-style (glass::Style: 0 frosted, 1 liquid, 2 clear, 3 tinted), edge-reflection (1 = on),
  // liquid-tint and liquid-blur (0..1).
  void setGlassStyle(std::uint32_t style, bool edgeReflection, double liquidTint, double liquidBlur);
  // org.gnome.desktop.wm.preferences button-layout: GTK and Chromium title bars show minimize, maximize and close
  // on the same side as the compositor's title bars.
  void setButtonsLeft(bool left);

private:
  SessionBus& m_bus;
  std::unique_ptr<sdbus::IObject> m_object;
  // org.freedesktop.appearance color-scheme: 0 no preference, 1 prefer dark, 2 prefer light.
  std::uint32_t m_colorScheme = 0;
  // Out of range (-1) until the shell sets an accent: the spec's "no preference".
  std::array<double, 3> m_accent{-1.0, -1.0, -1.0};
  std::uint32_t m_contrast = 0;
  std::uint32_t m_reducedMotion = 0;
  double m_uiScale = 1.0;
  double m_textScale = 1.0;
  double m_cornerRadiusScale = 1.0;
  std::string m_language;
  double m_glass = 1.0;
  double m_frost = 1.0;
  double m_liquid = 0.5;
  std::uint32_t m_reduceTransparency = 0;
  std::uint32_t m_compositorBlur = 0;
  std::uint32_t m_glassStyle = 0;
  std::uint32_t m_edgeReflection = 1;
  double m_liquidTint = 0.15;
  double m_liquidBlur = 0.1;
  std::string m_buttonLayout = ":minimize,maximize,close";

  void emitChanged(const char* ns, const char* key, const sdbus::Variant& value);
  [[nodiscard]] sdbus::Variant accentVariant() const;
};
