#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

class SessionBus;

namespace sdbus {
  class IObject;
  class Variant;
}

// Serves org.freedesktop.impl.portal.Settings under org.freedesktop.impl.portal.desktop.andromeda.shell, so apps follow
// the shell's appearance through xdg-desktop-portal. Setters stage values; publish() makes them visible: it emits
// SettingChanged for each changed key, then org.andromeda.appearance `theme` once, an a{sv} of {instance: t,
// generation: t, <namespace>: a{sv}}. The table holds each org.freedesktop.appearance and org.andromeda.appearance key
// the shell has set, so an app keeps its values for keys a restarted shell has not set yet. `instance` differs per
// shell process; `generation` grows by one per publish within it. Read and ReadAll serve the published values only.
class SettingsPortal {
public:
  explicit SettingsPortal(SessionBus& bus);
  ~SettingsPortal();

  SettingsPortal(const SettingsPortal&) = delete;
  SettingsPortal& operator=(const SettingsPortal&) = delete;

  // org.freedesktop.appearance color-scheme.
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
  // Publishes the staged values as one generation; nothing when no value changed.
  void publish();

  struct Values {
    // org.freedesktop.appearance color-scheme: 0 no preference, 1 prefer dark, 2 prefer light.
    std::uint32_t colorScheme = 0;
    // Out of range (-1) until the shell sets an accent: the spec's "no preference".
    std::array<double, 3> accent{-1.0, -1.0, -1.0};
    std::uint32_t contrast = 0;
    std::uint32_t reducedMotion = 0;
    double uiScale = 1.0;
    double textScale = 1.0;
    double cornerRadiusScale = 1.0;
    std::string language;
    double glass = 1.0;
    double frost = 1.0;
    double liquid = 0.5;
    std::uint32_t reduceTransparency = 0;
    std::uint32_t compositorBlur = 0;
    std::uint32_t glassStyle = 0;
    std::uint32_t edgeReflection = 1;
    double liquidTint = 0.15;
    double liquidBlur = 0.1;
    std::string buttonLayout = ":minimize,maximize,close";
    bool operator==(const Values&) const = default;
  };

private:
  SessionBus& m_bus;
  std::unique_ptr<sdbus::IObject> m_object;
  Values m_staged;
  Values m_published;
  // One bit per key (kSettings order in the .cpp): the shell has set it.
  std::uint32_t m_stagedKeys = 0;
  std::uint32_t m_publishedKeys = 0;
  // The wall clock in ns at construction.
  std::uint64_t m_instance = 0;
  std::uint64_t m_generation = 0;

  void stage(std::string_view key);
  void emitChanged(const char* ns, const char* key, const sdbus::Variant& value);
  [[nodiscard]] sdbus::Variant themeVariant() const;
};
