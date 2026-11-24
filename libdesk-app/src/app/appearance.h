#pragma once

#include "protocols/glass_material.h"

#include <array>
#include <functional>
#include <memory>
#include <string>

class SessionBus;
namespace sdbus {
  class IProxy;
  class Variant;
}

// Follows the desktop's light/dark choice, accent and reduced motion for an app process: org.freedesktop.appearance
// color-scheme, accent-color and reduced-motion from the Settings portal (the public frontend, else the Andromeda shell
// backend directly), and battery saver from the PowerProfileMonitor portal. Applies them to the libdesk-ui palette and
// MotionService. Also org.andromeda.appearance ui-scale and text-scale for the windows and text. `changed` runs
// after each change so windows rebuild.
class Appearance {
public:
  Appearance(SessionBus& bus, std::function<void()> changed);
  ~Appearance();
  // Reduced motion or battery saver: decorative motion (theme scenes) stays still.
  [[nodiscard]] bool holdMotion() const noexcept { return m_reducedMotion || m_powerSaver; }
  // org.andromeda.appearance ui-scale and text-scale from the shell; 1 until it publishes them.
  [[nodiscard]] float uiScale() const noexcept { return static_cast<float>(m_uiScale); }
  [[nodiscard]] float textScale() const noexcept { return static_cast<float>(m_textScale); }
  Appearance(const Appearance&) = delete;
  Appearance& operator=(const Appearance&) = delete;

private:
  void read(bool backend);
  // Stores one setting; false for a namespace, key or type this class does not follow.
  bool take(const std::string& ns, const std::string& key, const sdbus::Variant& value);
  void apply();

  SessionBus& m_bus;
  std::function<void()> m_changed;
  std::unique_ptr<sdbus::IProxy> m_portal;
  std::unique_ptr<sdbus::IProxy> m_backend;
  std::unique_ptr<sdbus::IProxy> m_power;
  bool m_light = false;
  bool m_hasAccent = false;
  bool m_reducedMotion = false;
  bool m_powerSaver = false;
  double m_accent[3] = {0.0, 0.0, 0.0};
  double m_uiScale = 1.0;
  double m_textScale = 1.0;
  double m_cornerRadiusScale = 1.0;
  std::string m_language; // empty: the locale environment
  // Style::setGlass inputs; the defaults match Style's.
  glass::Settings m_glass;
  bool m_compositorBlur = false;
  struct Applied {
    bool light = false;
    bool hasAccent = false;
    bool reducedMotion = false;
    bool powerSaver = false;
    std::array<double, 3> accent{};
    double uiScale = 1.0;
    double textScale = 1.0;
    double cornerRadiusScale = 1.0;
    glass::Settings glass;
    bool compositorBlur = false;
    std::string language;
    bool operator==(const Applied&) const = default;
  };
  Applied m_applied; // what windows last built with; the defaults match the startup palette
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
