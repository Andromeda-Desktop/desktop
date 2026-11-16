#pragma once

#include <array>
#include <functional>
#include <memory>

class SessionBus;
namespace sdbus {
  class IProxy;
}

// Follows the desktop's light/dark choice, accent and reduced motion for an app process: org.freedesktop.appearance
// color-scheme, accent-color and reduced-motion from the Settings portal (the public frontend, else the Andromeda shell
// backend directly), and battery saver from the PowerProfileMonitor portal. Applies them to the libdesk-ui palette and
// MotionService; `changed` runs after each change so windows repaint.
class Appearance {
public:
  Appearance(SessionBus& bus, std::function<void()> changed);
  ~Appearance();
  // Reduced motion or battery saver: decorative motion (theme scenes) stays still.
  [[nodiscard]] bool holdMotion() const noexcept { return m_reducedMotion || m_powerSaver; }
  Appearance(const Appearance&) = delete;
  Appearance& operator=(const Appearance&) = delete;

private:
  void read(bool backend);
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
  struct Applied {
    bool light = false;
    bool hasAccent = false;
    bool reducedMotion = false;
    bool powerSaver = false;
    std::array<double, 3> accent{};
    bool operator==(const Applied&) const = default;
  };
  Applied m_applied; // what windows last built with; the defaults match the startup palette
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
