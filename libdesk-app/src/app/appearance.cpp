#include "app/appearance.h"

#include "core/log.h"
#include "render/animation/motion_service.h"
#include "dbus/session_bus.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <sdbus-c++/sdbus-c++.h>

#include <algorithm>

namespace {
  constexpr Logger kLog("appearance");
  const sdbus::ServiceName kPortal{"org.freedesktop.portal.Desktop"};
  const sdbus::ServiceName kBackend{"org.freedesktop.impl.portal.desktop.andromeda.shell"};
  const sdbus::ObjectPath kPath{"/org/freedesktop/portal/desktop"};
  constexpr auto kPortalSettings = "org.freedesktop.portal.Settings";
  constexpr auto kBackendSettings = "org.freedesktop.impl.portal.Settings";
  constexpr auto kNamespace = "org.freedesktop.appearance";
  constexpr auto kAndromeda = "org.andromeda.appearance";
  constexpr auto kPowerMonitor = "org.freedesktop.portal.PowerProfileMonitor";
  constexpr auto kPowerSaverProperty = "power-saver-enabled";
  constexpr std::uint32_t kPreferLight = 2; // color-scheme: 0 no preference, 1 dark, 2 light

  // The frontend's Read wraps the value in one more variant; ReadOne and the backend do not.
  sdbus::Variant unwrap(sdbus::Variant v) {
    while (v.containsValueOfType<sdbus::Variant>()) {
      v = v.get<sdbus::Variant>();
    }
    return v;
  }
} // namespace

Appearance::Appearance(SessionBus& bus, std::function<void()> changed) : m_bus(bus), m_changed(std::move(changed)) {
  const auto onChanged = [this](const std::string& ns, const std::string& key, const sdbus::Variant& raw) {
    if (take(ns, key, unwrap(raw))) {
      apply();
    }
  };
  try {
    m_portal = sdbus::createProxy(m_bus.connection(), kPortal, kPath);
    m_portal->uponSignal("SettingChanged").onInterface(kPortalSettings).call(onChanged);
    m_backend = sdbus::createProxy(m_bus.connection(), kBackend, kPath);
    m_backend->uponSignal("SettingChanged").onInterface(kBackendSettings).call(onChanged);
    m_power = sdbus::createProxy(m_bus.connection(), kPortal, kPath);
    m_power->uponSignal("PropertiesChanged")
        .onInterface("org.freedesktop.DBus.Properties")
        .call([this](const std::string& iface, const std::map<std::string, sdbus::Variant>& changed,
                     const std::vector<std::string>& /*invalidated*/) {
          if (const auto it = changed.find(kPowerSaverProperty); iface == kPowerMonitor && it != changed.end()
                                                                   && it->second.containsValueOfType<bool>()) {
            m_powerSaver = it->second.get<bool>();
            apply();
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("settings portal: {}", e.getMessage());
    return;
  }
  read(false);
  m_power->callMethodAsync("Get")
      .onInterface("org.freedesktop.DBus.Properties")
      .withArguments(std::string(kPowerMonitor), std::string(kPowerSaverProperty))
      .uponReplyInvoke([this, alive = std::weak_ptr<bool>(m_alive)](std::optional<sdbus::Error> error, sdbus::Variant value) {
        if (!alive.expired() && !error.has_value() && value.containsValueOfType<bool>() && value.get<bool>()) {
          m_powerSaver = true;
          apply();
        }
      });
}

Appearance::~Appearance() = default;

void Appearance::read(bool backend) {
  sdbus::IProxy* proxy = backend ? m_backend.get() : m_portal.get();
  const std::pair<const char*, const char*> keys[] = {
      {kNamespace, "color-scheme"}, {kNamespace, "accent-color"}, {kNamespace, "reduced-motion"},
      {kAndromeda, "ui-scale"},     {kAndromeda, "text-scale"},     {kAndromeda, "glass"},
      {kAndromeda, "frost"},        {kAndromeda, "liquid"},       {kAndromeda, "reduce-transparency"}, {kAndromeda, "compositor-blur"},
      {kAndromeda, "glass-style"},  {kAndromeda, "edge-reflection"}, {kAndromeda, "liquid-tint"},     {kAndromeda, "liquid-blur"},
  };
  for (const auto& [ns, key] : keys) {
    proxy->callMethodAsync("Read")
        .onInterface(backend ? kBackendSettings : kPortalSettings)
        .withArguments(std::string(ns), std::string(key))
        .uponReplyInvoke([this, alive = std::weak_ptr<bool>(m_alive), backend, ns = std::string(ns), key = std::string(key)](
                             std::optional<sdbus::Error> error, sdbus::Variant raw) {
          if (alive.expired()) {
            return;
          }
          if (error.has_value()) {
            // No portal frontend (a bare session, the E2E bus): ask the shell's backend once.
            if (!backend && key == "color-scheme" && error->getName() != "org.freedesktop.portal.Error.NotFound") {
              read(true);
            }
            return;
          }
          if (take(ns, key, unwrap(std::move(raw)))) {
            apply();
          }
        });
  }
}

bool Appearance::take(const std::string& ns, const std::string& key, const sdbus::Variant& value) {
  if (ns == kAndromeda
      && (key == "glass" || key == "frost" || key == "liquid" || key == "liquid-tint" || key == "liquid-blur")) {
    if (!value.containsValueOfType<double>()) {
      return false;
    }
    const auto unit = static_cast<float>(std::clamp(value.get<double>(), 0.0, 1.0));
    (key == "glass"         ? m_glass.tint
     : key == "frost"       ? m_glass.frost
     : key == "liquid"      ? m_glass.lens
     : key == "liquid-tint" ? m_glass.liquidTint
                            : m_glass.liquidBlur) = unit;
    return true;
  }
  if (ns == kAndromeda
      && (key == "reduce-transparency" || key == "compositor-blur" || key == "glass-style" || key == "edge-reflection")) {
    if (!value.containsValueOfType<std::uint32_t>()) {
      return false;
    }
    const bool on = value.get<std::uint32_t>() == 1;
    if (key == "glass-style") {
      m_glass.style = on ? glass::Style::Liquid : glass::Style::Frosted;
    } else if (key == "edge-reflection") {
      m_glass.edgeReflection = on;
    } else if (key == "reduce-transparency") {
      m_glass.reduceTransparency = on;
    } else {
      m_compositorBlur = on;
    }
    return true;
  }
  if (ns == kAndromeda) {
    if (!value.containsValueOfType<double>()) {
      return false;
    }
    const double scale = value.get<double>();
    if (scale < 0.5 || scale > 4.0) {
      return false;
    }
    if (key == "ui-scale") {
      m_uiScale = scale;
    } else if (key == "text-scale") {
      m_textScale = scale;
    } else {
      return false;
    }
    return true;
  }
  if (ns != kNamespace) {
    return false;
  }
  if (key == "color-scheme" && value.containsValueOfType<std::uint32_t>()) {
    m_light = value.get<std::uint32_t>() == kPreferLight;
  } else if (key == "accent-color" && value.containsValueOfType<sdbus::Struct<double, double, double>>()) {
    const auto [r, g, b] = value.get<sdbus::Struct<double, double, double>>();
    m_hasAccent = r >= 0.0 && r <= 1.0 && g >= 0.0 && g <= 1.0 && b >= 0.0 && b <= 1.0;
    m_accent[0] = r;
    m_accent[1] = g;
    m_accent[2] = b;
  } else if (key == "reduced-motion" && value.containsValueOfType<std::uint32_t>()) {
    m_reducedMotion = value.get<std::uint32_t>() == 1;
  } else {
    return false;
  }
  return true;
}

void Appearance::apply() {
  const auto paletteFor = [this](bool light) {
    Palette p = tokenPalette(light);
    if (m_hasAccent) {
      p.accent = rgba(static_cast<float>(m_accent[0]), static_cast<float>(m_accent[1]), static_cast<float>(m_accent[2]), 1.0F);
    }
    return p;
  };
  setDarkPalette(paletteFor(false));
  setLightPalette(paletteFor(true));
  setResolvedThemeLight(m_light);
  setPalette(paletteFor(m_light));
  MotionService::instance().setReduced(m_reducedMotion);
  Style::setGlass(m_glass, m_compositorBlur);
  kLog.debug("scheme {}{}", m_light ? "light" : "dark", m_hasAccent ? " accent" : "");
  const Applied now{
      m_light,   m_hasAccent, m_reducedMotion, m_powerSaver, {m_accent[0], m_accent[1], m_accent[2]},
      m_uiScale, m_textScale, m_glass,         m_compositorBlur
  };
  if (now == m_applied) {
    return;
  }
  m_applied = now;
  if (m_changed) {
    m_changed();
  }
}
