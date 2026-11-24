#include "dbus/portal/settings_portal.h"

#include "core/log.h"
#include "dbus/session_bus.h"

#include <map>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("settings-portal");

  const sdbus::ServiceName kBusName{"org.freedesktop.impl.portal.desktop.andromeda.shell"};
  const sdbus::ObjectPath kObjectPath{"/org/freedesktop/portal/desktop"};
  constexpr auto kInterface = "org.freedesktop.impl.portal.Settings";
  constexpr auto kAppearance = "org.freedesktop.appearance";
  constexpr auto kColorScheme = "color-scheme";
  constexpr auto kAccentColor = "accent-color";
  constexpr auto kContrast = "contrast";
  constexpr auto kReducedMotion = "reduced-motion";
  constexpr auto kAndromeda = "org.andromeda.appearance";
  constexpr auto kUiScale = "ui-scale";
  constexpr auto kGlass = "glass";
  constexpr auto kFrost = "frost";
  constexpr auto kLiquid = "liquid";
  constexpr auto kReduceTransparency = "reduce-transparency";
  constexpr auto kCompositorBlur = "compositor-blur";
  constexpr auto kGlassStyle = "glass-style";
  constexpr auto kEdgeReflection = "edge-reflection";
  constexpr auto kLiquidTint = "liquid-tint";
  constexpr auto kLiquidBlur = "liquid-blur";
  constexpr auto kTextScale = "text-scale";
  constexpr auto kCornerRadiusScale = "corner-radius-scale";
  constexpr auto kWmPreferences = "org.gnome.desktop.wm.preferences";
  constexpr auto kButtonLayout = "button-layout";
  constexpr std::uint32_t kVersion = 2;

  // A namespace pattern matches exactly, or as a prefix when it ends in '*'; no patterns means every namespace.
  bool wanted(const std::vector<std::string>& patterns, const std::string& ns) {
    if (patterns.empty()) {
      return true;
    }
    for (const std::string& pattern : patterns) {
      if (pattern == ns || (pattern.ends_with('*') && ns.starts_with(pattern.substr(0, pattern.size() - 1)))) {
        return true;
      }
    }
    return false;
  }

} // namespace

SettingsPortal::SettingsPortal(SessionBus& bus) : m_bus(bus) {
  m_object = sdbus::createObject(m_bus.connection(), kObjectPath);
  m_object
      ->addVTable(
          sdbus::registerMethod("ReadAll")
              .withInputParamNames("namespaces")
              .withOutputParamNames("value")
              .implementedAs([this](const std::vector<std::string>& namespaces) {
                std::map<std::string, std::map<std::string, sdbus::Variant>> all;
                if (wanted(namespaces, kAppearance)) {
                  all[kAppearance][kColorScheme] = sdbus::Variant{m_colorScheme};
                  all[kAppearance][kAccentColor] = accentVariant();
                  all[kAppearance][kContrast] = sdbus::Variant{m_contrast};
                  all[kAppearance][kReducedMotion] = sdbus::Variant{m_reducedMotion};
                }
                if (wanted(namespaces, kAndromeda)) {
                  all[kAndromeda][kUiScale] = sdbus::Variant{m_uiScale};
                  all[kAndromeda][kTextScale] = sdbus::Variant{m_textScale};
                  all[kAndromeda][kCornerRadiusScale] = sdbus::Variant{m_cornerRadiusScale};
                  all[kAndromeda][kGlass] = sdbus::Variant{m_glass};
                  all[kAndromeda][kFrost] = sdbus::Variant{m_frost};
                  all[kAndromeda][kLiquid] = sdbus::Variant{m_liquid};
                  all[kAndromeda][kReduceTransparency] = sdbus::Variant{m_reduceTransparency};
                  all[kAndromeda][kCompositorBlur] = sdbus::Variant{m_compositorBlur};
                  all[kAndromeda][kGlassStyle] = sdbus::Variant{m_glassStyle};
                  all[kAndromeda][kEdgeReflection] = sdbus::Variant{m_edgeReflection};
                  all[kAndromeda][kLiquidTint] = sdbus::Variant{m_liquidTint};
                  all[kAndromeda][kLiquidBlur] = sdbus::Variant{m_liquidBlur};
                }
                if (wanted(namespaces, kWmPreferences)) {
                  all[kWmPreferences][kButtonLayout] = sdbus::Variant{m_buttonLayout};
                }
                return all;
              }),
          sdbus::registerMethod("Read")
              .withInputParamNames("namespace", "key")
              .withOutputParamNames("value")
              .implementedAs([this](const std::string& ns, const std::string& key) {
                if (ns == kAppearance && key == kColorScheme) {
                  return sdbus::Variant{m_colorScheme};
                }
                if (ns == kAppearance && key == kAccentColor) {
                  return accentVariant();
                }
                if (ns == kAppearance && key == kContrast) {
                  return sdbus::Variant{m_contrast};
                }
                if (ns == kAppearance && key == kReducedMotion) {
                  return sdbus::Variant{m_reducedMotion};
                }
                if (ns == kAndromeda && key == kUiScale) {
                  return sdbus::Variant{m_uiScale};
                }
                if (ns == kAndromeda && key == kTextScale) {
                  return sdbus::Variant{m_textScale};
                }
                if (ns == kAndromeda && key == kCornerRadiusScale) {
                  return sdbus::Variant{m_cornerRadiusScale};
                }
                if (ns == kAndromeda && key == kGlass) {
                  return sdbus::Variant{m_glass};
                }
                if (ns == kAndromeda && key == kFrost) {
                  return sdbus::Variant{m_frost};
                }
                if (ns == kAndromeda && key == kLiquid) {
                  return sdbus::Variant{m_liquid};
                }
                if (ns == kAndromeda && key == kReduceTransparency) {
                  return sdbus::Variant{m_reduceTransparency};
                }
                if (ns == kAndromeda && key == kCompositorBlur) {
                  return sdbus::Variant{m_compositorBlur};
                }
                if (ns == kAndromeda && key == kGlassStyle) {
                  return sdbus::Variant{m_glassStyle};
                }
                if (ns == kAndromeda && key == kEdgeReflection) {
                  return sdbus::Variant{m_edgeReflection};
                }
                if (ns == kAndromeda && key == kLiquidTint) {
                  return sdbus::Variant{m_liquidTint};
                }
                if (ns == kAndromeda && key == kLiquidBlur) {
                  return sdbus::Variant{m_liquidBlur};
                }
                if (ns == kWmPreferences && key == kButtonLayout) {
                  return sdbus::Variant{m_buttonLayout};
                }
                throw sdbus::Error(
                    sdbus::Error::Name{"org.freedesktop.portal.Error.NotFound"}, "Requested setting not found"
                );
              }),
          sdbus::registerSignal("SettingChanged")
              .withParameters<std::string, std::string, sdbus::Variant>("namespace", "key", "value"),
          sdbus::registerProperty("version").withGetter([]() { return kVersion; })
      )
      .forInterface(kInterface);
  m_bus.connection().requestName(kBusName);
  kLog.info("serving {} as {}", kInterface, static_cast<const std::string&>(kBusName));
}

SettingsPortal::~SettingsPortal() {
  try {
    m_bus.connection().releaseName(kBusName);
  } catch (const sdbus::Error& e) {
    kLog.debug("release {} failed: {}", static_cast<const std::string&>(kBusName), e.what());
  }
}

void SettingsPortal::setDark(bool dark) {
  const std::uint32_t scheme = dark ? 1 : 2;
  if (scheme == m_colorScheme) {
    return;
  }
  m_colorScheme = scheme;
  emitChanged(kAppearance, kColorScheme, sdbus::Variant{scheme});
}

void SettingsPortal::setAccent(double r, double g, double b) {
  const std::array<double, 3> accent{r, g, b};
  if (accent == m_accent) {
    return;
  }
  m_accent = accent;
  emitChanged(kAppearance, kAccentColor, accentVariant());
}

void SettingsPortal::setHighContrast(bool high) {
  const std::uint32_t contrast = high ? 1 : 0;
  if (contrast == m_contrast) {
    return;
  }
  m_contrast = contrast;
  emitChanged(kAppearance, kContrast, sdbus::Variant{contrast});
}

void SettingsPortal::setReducedMotion(bool reduce) {
  const std::uint32_t reduced = reduce ? 1 : 0;
  if (reduced == m_reducedMotion) {
    return;
  }
  m_reducedMotion = reduced;
  emitChanged(kAppearance, kReducedMotion, sdbus::Variant{reduced});
}

void SettingsPortal::setButtonsLeft(bool left) {
  std::string layout = left ? "close,minimize,maximize:" : ":minimize,maximize,close";
  if (layout == m_buttonLayout) {
    return;
  }
  m_buttonLayout = std::move(layout);
  emitChanged(kWmPreferences, kButtonLayout, sdbus::Variant{m_buttonLayout});
}

void SettingsPortal::setScales(double uiScale, double textScale, double cornerRadiusScale) {
  if (uiScale != m_uiScale) {
    m_uiScale = uiScale;
    emitChanged(kAndromeda, kUiScale, sdbus::Variant{uiScale});
  }
  if (textScale != m_textScale) {
    m_textScale = textScale;
    emitChanged(kAndromeda, kTextScale, sdbus::Variant{textScale});
  }
  if (cornerRadiusScale != m_cornerRadiusScale) {
    m_cornerRadiusScale = cornerRadiusScale;
    emitChanged(kAndromeda, kCornerRadiusScale, sdbus::Variant{cornerRadiusScale});
  }
}

void SettingsPortal::setGlass(double glass, double frost, double liquid, bool reduceTransparency, bool compositorBlur) {
  if (glass != m_glass) {
    m_glass = glass;
    emitChanged(kAndromeda, kGlass, sdbus::Variant{glass});
  }
  if (frost != m_frost) {
    m_frost = frost;
    emitChanged(kAndromeda, kFrost, sdbus::Variant{frost});
  }
  if (liquid != m_liquid) {
    m_liquid = liquid;
    emitChanged(kAndromeda, kLiquid, sdbus::Variant{liquid});
  }
  const std::uint32_t reduce = reduceTransparency ? 1 : 0;
  if (reduce != m_reduceTransparency) {
    m_reduceTransparency = reduce;
    emitChanged(kAndromeda, kReduceTransparency, sdbus::Variant{reduce});
  }
  const std::uint32_t blur = compositorBlur ? 1 : 0;
  if (blur != m_compositorBlur) {
    m_compositorBlur = blur;
    emitChanged(kAndromeda, kCompositorBlur, sdbus::Variant{blur});
  }
}

void SettingsPortal::setGlassStyle(std::uint32_t style, bool edgeReflection, double liquidTint, double liquidBlur) {
  if (style != m_glassStyle) {
    m_glassStyle = style;
    emitChanged(kAndromeda, kGlassStyle, sdbus::Variant{style});
  }
  const std::uint32_t edge = edgeReflection ? 1 : 0;
  if (edge != m_edgeReflection) {
    m_edgeReflection = edge;
    emitChanged(kAndromeda, kEdgeReflection, sdbus::Variant{edge});
  }
  if (liquidTint != m_liquidTint) {
    m_liquidTint = liquidTint;
    emitChanged(kAndromeda, kLiquidTint, sdbus::Variant{liquidTint});
  }
  if (liquidBlur != m_liquidBlur) {
    m_liquidBlur = liquidBlur;
    emitChanged(kAndromeda, kLiquidBlur, sdbus::Variant{liquidBlur});
  }
}

sdbus::Variant SettingsPortal::accentVariant() const {
  return sdbus::Variant{sdbus::Struct<double, double, double>{m_accent[0], m_accent[1], m_accent[2]}};
}

void SettingsPortal::emitChanged(const char* ns, const char* key, const sdbus::Variant& value) {
  m_object->emitSignal("SettingChanged").onInterface(kInterface).withArguments(std::string(ns), std::string(key), value);
}
